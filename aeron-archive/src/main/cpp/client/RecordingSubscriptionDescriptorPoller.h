/*
 * Copyright 2014-2019 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef AERON_RECORDING_SUBSCRIPTION_DESCRIPTOR_POLLER_H
#define AERON_RECORDING_SUBSCRIPTION_DESCRIPTOR_POLLER_H

#include "Aeron.h"
#include "ControlledFragmentAssembler.h"

namespace aeron {
namespace archive {
namespace client
{

typedef std::function<void(
    std::int64_t controlSessionId,
    std::int64_t correlationId,
    std::int64_t subscriptionId,
    std::int32_t streamId,
    const std::string& strippedChannel)> recording_subscription_descriptor_consumer_t;

class RecordingSubscriptionDescriptorPoller
{
public:
    RecordingSubscriptionDescriptorPoller(
        std::shared_ptr<Subscription> subscription,
        const exception_handler_t& errorHandler,
        std::int64_t controlSessionId,
        int fragmentLimit = 10);

    inline int poll()
    {
        m_isDispatchComplete = false;

        return m_subscription->controlledPoll(m_fragmentHandler, m_fragmentLimit);
    }

    inline std::shared_ptr<Subscription> subscription()
    {
        return m_subscription;
    }

    inline std::int64_t controlSessionId()
    {
        return m_controlSessionId;
    }

    inline bool isDispatchComplete()
    {
        return m_isDispatchComplete;
    }

    inline std::int32_t remainingSubscriptionCount()
    {
        return m_remainingSubscriptionCount;
    }

    inline void reset(
        std::int64_t correlationId,
        std::int32_t subscriptionCount,
        const recording_subscription_descriptor_consumer_t& consumer)
    {
        m_correlationId = correlationId;
        m_remainingSubscriptionCount = subscriptionCount;
        m_consumer = consumer;
        m_isDispatchComplete = false;
    }

    ControlledPollAction onFragment(AtomicBuffer& buffer, util::index_t offset, util::index_t length, Header& header);

private:
    ControlledFragmentAssembler m_fragmentAssembler;
    controlled_poll_fragment_handler_t m_fragmentHandler;
    exception_handler_t m_errorHandler;
    recording_subscription_descriptor_consumer_t m_consumer = nullptr;
    std::shared_ptr<Subscription> m_subscription;

    const std::int64_t m_controlSessionId;
    const int m_fragmentLimit;

    std::int64_t m_correlationId = -1;
    std::int32_t m_remainingSubscriptionCount = 0;
    bool m_isDispatchComplete = false;
};

}}}
#endif //AERON_RECORDING_SUBSCRIPTION_DESCRIPTOR_POLLER_H
