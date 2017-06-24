#pragma once
#include "StrandOwnerLifetimeExtender.h"
#include "catapult/utils/TimeSpan.h"
#include <boost/asio/steady_timer.hpp>
#include <memory>

namespace catapult { namespace thread {

	/// Wraps a callback with a timer using an explicit strand and ensures deterministic shutdown by using
	/// enable_shared_from_this.
	template<typename TCallback, typename... TCallbackArgs>
	class StrandedTimedCallback
			: public std::enable_shared_from_this<StrandedTimedCallback<TCallback, TCallbackArgs...>> {
	private:
		using TimeoutHandlerType = std::function<void ()>;

	private:
		/// Wraps a callback with a timer using an implicit strand.
		template<typename TCallbackWrapper>
		class BasicTimedCallback {
		public:
			BasicTimedCallback(
					TCallbackWrapper& wrapper,
					boost::asio::io_service& service,
					const TCallback& callback,
					TCallbackArgs&&... timeoutArgs)
					: m_wrapper(wrapper)
					, m_callback(callback)
					, m_timeoutArgs(std::forward<TCallbackArgs>(timeoutArgs)...)
					, m_timer(service)
					, m_isCallbackInvoked(false)
					, m_isTimedOut(false)
			{}

		public:
			void setTimeout(const utils::TimeSpan& timeout) {
				m_timer.expires_from_now(std::chrono::milliseconds(timeout.millis()));
				m_timer.async_wait(m_wrapper.wrap([this](const auto&) {
					this->handleTimedOut();
				}));
			}

			void setTimeoutHandler(const TimeoutHandlerType& handler) {
				// if the callback was not already executed, save the handler for later
				if (!m_isCallbackInvoked) {
					m_timeoutHandler = handler;
					return;
				}

				// if this callback is already timed out, immediately execute the handler before returning
				if (m_isTimedOut)
					handler();
			}

			bool callback(const std::tuple<TCallbackArgs...>& args) {
				if (m_isCallbackInvoked)
					return false;

				m_isCallbackInvoked = true;

				// destroy the callback after this function returns
				TCallback callback;
				m_callback.swap(callback);

				callback(std::get<TCallbackArgs>(args)...);
				m_timer.cancel();
				return true;
			}

		private:
			void handleTimedOut() {
				// destroy the timeout handler after this function returns
				TimeoutHandlerType timeoutHandler;
				m_timeoutHandler.swap(timeoutHandler);

				if (!callback(m_timeoutArgs))
					return;

				m_isTimedOut = true;
				if (timeoutHandler)
					timeoutHandler();
			}

		private:
			TCallbackWrapper& m_wrapper;
			TCallback m_callback;
			std::tuple<TCallbackArgs...> m_timeoutArgs;

			boost::asio::steady_timer m_timer;
			bool m_isCallbackInvoked;
			bool m_isTimedOut;
			TimeoutHandlerType m_timeoutHandler;
		};

	public:
		/// Creates a timed callback by wrapping \a callback with a timed callback using \a service.
		/// On a timeout, the callback is invoked with \a timeoutArgs.
		StrandedTimedCallback(
				boost::asio::io_service& service,
				const TCallback& callback,
				TCallbackArgs&&... timeoutArgs)
				: m_impl(*this, service, callback, std::forward<TCallbackArgs>(timeoutArgs)...)
				, m_strand(service)
				, m_strandWrapper(m_strand)
		{}

	public:
		/// Sets the timeout to \a timeout (starting from now).
		void setTimeout(const utils::TimeSpan& timeout) {
			post([timeout](auto& impl) { impl.setTimeout(timeout); });
		}

		/// Sets the timeout handler to \a timeoutHandler.
		void setTimeoutHandler(const TimeoutHandlerType& handler) {
			post([handler](auto& impl) { impl.setTimeoutHandler(handler); });
		}

		/// Invokes the wrapped callback with \a args.
		template<typename... TArgs>
		void callback(TArgs&&... args) {
			// note that this function needs to be TArgs instead of TCallbackArgs so that it can be called
			// with different qualifiers - e.g. when constructing, a timeoutArg might be passed by value
			// but here it might be passed by (const) reference
			post([args = std::make_tuple(std::forward<TArgs>(args)...)](auto& impl) {
				impl.callback(args);
			});
		}

	public:
		template<typename THandler>
		auto wrap(THandler handler) {
			return m_strandWrapper.wrap(this->shared_from_this(), handler);
		}

	private:
		template<typename THandler>
		void post(THandler handler) {
			return m_strandWrapper.post(this->shared_from_this(), [handler](const auto& pThis) {
				handler(pThis->m_impl);
			});
		}

	private:
		BasicTimedCallback<StrandedTimedCallback> m_impl;
		boost::asio::strand m_strand;
		StrandOwnerLifetimeExtender<StrandedTimedCallback> m_strandWrapper;
	};

	/// Wraps \a callback with a timed callback using \a service.
	/// On a timeout, the callback is invoked with \a timeoutArgs.
	template<typename TCallback, typename... TCallbackArgs>
	auto MakeTimedCallback(boost::asio::io_service& service, TCallback callback, TCallbackArgs&&... timeoutArgs) {
		return std::make_shared<StrandedTimedCallback<TCallback, TCallbackArgs...>>(
				service,
				callback,
				std::forward<TCallbackArgs>(timeoutArgs)...);
	}
}}