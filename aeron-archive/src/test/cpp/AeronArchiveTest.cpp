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

#if defined(__linux__) || defined(Darwin)
#include <unistd.h>
#include <signal.h>
#include <ftw.h>
#include <stdio.h>
#else
#error "must spawn Java archive per test"
#endif

#include <chrono>
#include <thread>
#include <iostream>
#include <iosfwd>
#include <vector>

#include <gtest/gtest.h>

#include "client/AeronArchive.h"
#include "client/RecordingEventsAdapter.h"
#include "client/RecordingPos.h"

using namespace aeron;
using namespace aeron::archive::client;

class AeronArchiveTest : public testing::Test
{
public:
    ~AeronArchiveTest()
    {
        if (m_debug)
        {
            std::cout << m_stream.str();
        }
    }

    static int unlink_func(const char *path, const struct stat *sb, int type_flag, struct FTW *ftw)
    {
        if (remove(path) != 0)
        {
            perror("remove");
        }

        return 0;
    }

    static int deleteDir(const std::string& dirname)
    {
        return nftw(dirname.c_str(), unlink_func, 64, FTW_DEPTH | FTW_PHYS);
    }

    void SetUp() final
    {
        m_pid = ::fork();
        if (0 == m_pid)
        {
            if (::execl(m_java.c_str(),
                "java",
                "-Daeron.dir.delete.on.start=true",
                "-Daeron.archive.dir.delete.on.start=true",
                "-Daeron.archive.max.catalog.entries=1024",
                "-Daeron.threading.mode=INVOKER",
                "-Daeron.archive.threading.mode=SHARED",
                "-Daeron.archive.file.sync.level=0",
                "-Daeron.spies.simulate.connection=false",
                "-Daeron.mtu.length=4k",
                "-Daeron.term.buffer.sparse.file=true",
                ("-Daeron.archive.dir=" + m_archiveDir).c_str(),
                "-cp",
                m_aeronAllJar.c_str(),
                "io.aeron.archive.ArchivingMediaDriver",
                NULL) < 0)
            {
                perror("execl");
                ::exit(EXIT_FAILURE);
            }
        }

        m_stream << "ArchivingMediaDriver PID " << std::to_string(m_pid) << std::endl;
    }

    void TearDown() final
    {
        if (0 != m_pid)
        {
            int result = ::kill(m_pid, SIGINT);
            m_stream << "Shutting down PID " << m_pid << " " << result << std::endl;
            if (result < 0)
            {
                perror("kill");
            }

            ::wait(NULL);

            m_stream << "Deleting " << aeron::Context::defaultAeronPath() << std::endl;
            deleteDir(aeron::Context::defaultAeronPath());
            m_stream << "Deleting " << m_archiveDir << std::endl;
            deleteDir(m_archiveDir);
        }
    }

    std::shared_ptr<Publication> addPublication(
        Aeron& aeron, const std::string& channel, std::int32_t streamId)
    {
        std::int64_t publicationId = aeron.addPublication(channel, streamId);
        std::shared_ptr<Publication> publication = aeron.findPublication(publicationId);
        aeron::concurrent::YieldingIdleStrategy idle;
        while (!publication)
        {
            idle.idle();
            publication = aeron.findPublication(publicationId);
        }

        return publication;
    }

    std::shared_ptr<Subscription> addSubscription(
        Aeron& aeron, const std::string& channel, std::int32_t streamId)
    {
        std::int64_t subscriptionId = aeron.addSubscription(channel, streamId);
        std::shared_ptr<Subscription> subscription = aeron.findSubscription(subscriptionId);
        aeron::concurrent::YieldingIdleStrategy idle;
        while (!subscription)
        {
            idle.idle();
            subscription = aeron.findSubscription(subscriptionId);
        }

        return subscription;
    }

    std::int32_t getRecordingCounterId(std::int32_t sessionId, CountersReader& countersReader)
    {
        std::int32_t counterId;
        while (CountersReader::NULL_COUNTER_ID ==
            (counterId = RecordingPos::findCounterIdBySession(countersReader, sessionId)))
        {
            std::this_thread::yield();
        }

        return counterId;
    }

    void offerMessages(Publication& publication, std::size_t messageCount, const std::string& messagePrefix)
    {
        BufferClaim bufferClaim;
        aeron::concurrent::YieldingIdleStrategy idle;

        for (std::size_t i = 0; i < messageCount; i++)
        {
            const std::string message = messagePrefix + std::to_string(i);
            while (publication.tryClaim(static_cast<util::index_t>(message.length()), bufferClaim) < 0)
            {
                idle.idle();
            }

            bufferClaim.buffer().putStringWithoutLength(bufferClaim.offset(), message);
            bufferClaim.commit();
        }
    }

    void consumeMessages(Subscription& subscription, std::size_t messageCount, const std::string& messagePrefix)
    {
        std::size_t received = 0;
        aeron::concurrent::YieldingIdleStrategy idle;

        fragment_handler_t handler =
            [&](AtomicBuffer& buffer, util::index_t offset, util::index_t length, Header& header)
            {
                const std::string expected = messagePrefix + std::to_string(received);
                const std::string actual = buffer.getStringWithoutLength(offset, static_cast<std::size_t>(length));

                EXPECT_EQ(expected, actual);

                received++;
            };

        while (received < messageCount)
        {
            if (0 == subscription.poll(handler, m_fragmentLimit))
            {
                idle.idle();
            }
        }

        ASSERT_EQ(received, messageCount);
    }

protected:
    const std::string m_java = JAVA_EXECUTABLE;
    const std::string m_aeronAllJar = AERON_ALL_JAR;
    const std::string m_archiveDir = ARCHIVE_DIR;

    const std::string m_recordingChannel = "aeron:udp?endpoint=localhost:3333|term-length=65536";
    const std::int32_t m_recordingStreamId = 33;
    const std::string m_replayChannel = "aeron:udp?endpoint=localhost:6666";
    const std::int32_t m_replayStreamId = 66;

    const int m_fragmentLimit = 10;

    pid_t m_pid = 0;

    std::ostringstream m_stream;
    bool m_debug = false;
};

TEST_F(AeronArchiveTest, shouldSpinUpArchiveAndShutdown)
{
    m_stream << m_java << std::endl;
    m_stream << m_aeronAllJar << std::endl;
    m_stream << m_archiveDir << std::endl;

    std::this_thread::sleep_for(std::chrono::seconds(1));
}

TEST_F(AeronArchiveTest, shouldBeAbleToConnectToArchive)
{
    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();
}

TEST_F(AeronArchiveTest, shouldBeAbleToConnectToArchiveViaAsync)
{
    std::shared_ptr<AeronArchive::AsyncConnect> asyncConnect = AeronArchive::asyncConnect();
    aeron::concurrent::YieldingIdleStrategy idle;

    std::shared_ptr<AeronArchive> aeronArchive = asyncConnect->poll();
    while (!aeronArchive)
    {
        idle.idle();
        aeronArchive = asyncConnect->poll();
    }
}

TEST_F(AeronArchiveTest, shouldRecordPublicationAndFindRecording)
{
    const std::string messagePrefix = "Message ";
    const std::size_t messageCount = 10;
    std::int32_t sessionId = aeron::NULL_VALUE;
    std::int64_t recordingIdFromCounter = aeron::NULL_VALUE;
    std::int64_t stopPosition = aeron::NULL_VALUE;

    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();

    const std::int64_t subscriptionId = aeronArchive->startRecording(
        m_recordingChannel, m_recordingStreamId, AeronArchive::SourceLocation::LOCAL);

    {
        std::shared_ptr<Publication> publication = addPublication(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);
        std::shared_ptr<Subscription> subscription = addSubscription(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);

        sessionId = publication->sessionId();

        CountersReader& countersReader = aeronArchive->context().aeron()->countersReader();
        const std::int32_t counterId = getRecordingCounterId(sessionId, countersReader);
        recordingIdFromCounter = RecordingPos::getRecordingId(countersReader, counterId);

        offerMessages(*publication, messageCount, messagePrefix);
        consumeMessages(*subscription, messageCount, messagePrefix);

        stopPosition = publication->position();

        aeron::concurrent::YieldingIdleStrategy idle;
        while (countersReader.getCounterValue(counterId) < stopPosition)
        {
            idle.idle();
        }

        EXPECT_EQ(aeronArchive->getRecordingPosition(recordingIdFromCounter), stopPosition);
        EXPECT_EQ(aeronArchive->getStopPosition(recordingIdFromCounter), aeron::NULL_VALUE);
    }

    aeronArchive->stopRecording(subscriptionId);

    const std::int64_t recordingId = aeronArchive->findLastMatchingRecording(
        0, "endpoint=localhost:3333", m_recordingStreamId, sessionId);

    EXPECT_EQ(recordingIdFromCounter, recordingId);
    EXPECT_EQ(aeronArchive->getStopPosition(recordingIdFromCounter), stopPosition);

    const std::int32_t count = aeronArchive->listRecording(
        recordingId,
        [&](std::int64_t controlSessionId, std::int64_t correlationId, std::int64_t recordingId1,
            std::int64_t startTimestamp, std::int64_t stopTimestamp, std::int64_t startPosition,
            std::int64_t newStopPosition, std::int32_t initialTermId, std::int32_t segmentFileLength,
            std::int32_t termBufferLength, std::int32_t mtuLength, std::int32_t sessionId1, std::int32_t streamId,
            const std::string& strippedChannel, const std::string& originalChannel, const std::string& sourceIdentity)
        {
            EXPECT_EQ(recordingId, recordingId1);
            EXPECT_EQ(streamId, m_recordingStreamId);
        });

    EXPECT_EQ(count, 1);
}

TEST_F(AeronArchiveTest, shouldRecordThenReplay)
{
    const std::string messagePrefix = "Message ";
    const std::size_t messageCount = 10;
    std::int32_t sessionId = aeron::NULL_VALUE;
    std::int64_t recordingIdFromCounter = aeron::NULL_VALUE;
    std::int64_t stopPosition = aeron::NULL_VALUE;

    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();

    const std::int64_t subscriptionId = aeronArchive->startRecording(
        m_recordingChannel, m_recordingStreamId, AeronArchive::SourceLocation::LOCAL);

    {
        std::shared_ptr<Publication> publication = addPublication(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);
        std::shared_ptr<Subscription> subscription = addSubscription(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);

        sessionId = publication->sessionId();

        CountersReader& countersReader = aeronArchive->context().aeron()->countersReader();
        const std::int32_t counterId = getRecordingCounterId(sessionId, countersReader);
        recordingIdFromCounter = RecordingPos::getRecordingId(countersReader, counterId);

        offerMessages(*publication, messageCount, messagePrefix);
        consumeMessages(*subscription, messageCount, messagePrefix);

        stopPosition = publication->position();

        aeron::concurrent::YieldingIdleStrategy idle;
        while (countersReader.getCounterValue(counterId) < stopPosition)
        {
            idle.idle();
        }

        EXPECT_EQ(aeronArchive->getRecordingPosition(recordingIdFromCounter), stopPosition);
    }

    aeronArchive->stopRecording(subscriptionId);

    EXPECT_EQ(aeronArchive->getStopPosition(recordingIdFromCounter), stopPosition);

    const std::int64_t position = 0L;
    const std::int64_t length = stopPosition - position;

    {
        std::shared_ptr<Subscription> subscription = addSubscription(
            *aeronArchive->context().aeron(), m_replayChannel, m_replayStreamId);

        aeronArchive->startReplay(recordingIdFromCounter, position, length, m_replayChannel, m_replayStreamId);

        consumeMessages(*subscription, messageCount, messagePrefix);
        EXPECT_EQ(stopPosition, subscription->imageAtIndex(0).position());
    }
}

TEST_F(AeronArchiveTest, shouldRecordThenReplayThenTruncate)
{
    const std::string messagePrefix = "Message ";
    const std::size_t messageCount = 10;
    std::int32_t sessionId = aeron::NULL_VALUE;
    std::int64_t recordingIdFromCounter = aeron::NULL_VALUE;
    std::int64_t stopPosition = aeron::NULL_VALUE;

    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();

    const std::int64_t subscriptionId = aeronArchive->startRecording(
        m_recordingChannel, m_recordingStreamId, AeronArchive::SourceLocation::LOCAL);

    {
        std::shared_ptr<Publication> publication = addPublication(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);
        std::shared_ptr<Subscription> subscription = addSubscription(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);

        sessionId = publication->sessionId();

        CountersReader& countersReader = aeronArchive->context().aeron()->countersReader();
        const std::int32_t counterId = getRecordingCounterId(sessionId, countersReader);
        recordingIdFromCounter = RecordingPos::getRecordingId(countersReader, counterId);

        offerMessages(*publication, messageCount, messagePrefix);
        consumeMessages(*subscription, messageCount, messagePrefix);

        stopPosition = publication->position();

        aeron::concurrent::YieldingIdleStrategy idle;
        while (countersReader.getCounterValue(counterId) < stopPosition)
        {
            idle.idle();
        }

        EXPECT_EQ(aeronArchive->getRecordingPosition(recordingIdFromCounter), stopPosition);
        EXPECT_EQ(aeronArchive->getStopPosition(recordingIdFromCounter), aeron::NULL_VALUE);
    }

    aeronArchive->stopRecording(subscriptionId);

    const std::int64_t recordingId = aeronArchive->findLastMatchingRecording(
        0, "endpoint=localhost:3333", m_recordingStreamId, sessionId);

    EXPECT_EQ(recordingIdFromCounter, recordingId);
    EXPECT_EQ(aeronArchive->getStopPosition(recordingIdFromCounter), stopPosition);

    const std::int64_t position = 0L;
    const std::int64_t length = stopPosition - position;

    {
        std::shared_ptr<Subscription> subscription = aeronArchive->replay(
            recordingId, position, length, m_replayChannel, m_replayStreamId);

        consumeMessages(*subscription, messageCount, messagePrefix);
        EXPECT_EQ(stopPosition, subscription->imageAtIndex(0).position());
    }

    aeronArchive->truncateRecording(recordingId, position);

    const std::int32_t count = aeronArchive->listRecording(
        recordingId,
        [&](std::int64_t controlSessionId, std::int64_t correlationId, std::int64_t recordingId1,
            std::int64_t startTimestamp, std::int64_t stopTimestamp, std::int64_t startPosition,
            std::int64_t newStopPosition, std::int32_t initialTermId, std::int32_t segmentFileLength,
            std::int32_t termBufferLength, std::int32_t mtuLength, std::int32_t sessionId1, std::int32_t streamId,
            const std::string& strippedChannel, const std::string& originalChannel, const std::string& sourceIdentity)
        {
            EXPECT_EQ(startPosition, newStopPosition);
        });

    EXPECT_EQ(count, 1);
}

TEST_F(AeronArchiveTest, shouldRecordAndCancelReplayEarly)
{
    const std::string messagePrefix = "Message ";
    const std::size_t messageCount = 10;
    std::int64_t recordingId = aeron::NULL_VALUE;
    std::int64_t stopPosition = aeron::NULL_VALUE;

    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();

    {
        std::shared_ptr<Subscription> subscription = addSubscription(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);
        std::shared_ptr<Publication> publication = aeronArchive->addRecordedPublication(
            m_recordingChannel, m_recordingStreamId);

        CountersReader& countersReader = aeronArchive->context().aeron()->countersReader();
        const std::int32_t counterId = getRecordingCounterId(publication->sessionId(), countersReader);
        recordingId = RecordingPos::getRecordingId(countersReader, counterId);

        offerMessages(*publication, messageCount, messagePrefix);
        consumeMessages(*subscription, messageCount, messagePrefix);

        stopPosition = publication->position();

        aeron::concurrent::YieldingIdleStrategy idle;
        while (countersReader.getCounterValue(counterId) < stopPosition)
        {
            idle.idle();
        }

        EXPECT_EQ(aeronArchive->getRecordingPosition(recordingId), stopPosition);

        aeronArchive->stopRecording(publication);

        while (NULL_POSITION != aeronArchive->getRecordingPosition(recordingId))
        {
            idle.idle();
        }
    }

    const std::int64_t position = 0L;
    const std::int64_t length = stopPosition - position;

    const std::int64_t replaySessionId = aeronArchive->startReplay(
        recordingId, position, length, m_replayChannel, m_replayStreamId);

    aeronArchive->stopReplay(replaySessionId);
}

TEST_F(AeronArchiveTest, shouldReplayRecordingFromLateJoinPosition)
{
    const std::string messagePrefix = "Message ";
    const std::size_t messageCount = 10;

    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();

    const std::int64_t subscriptionId = aeronArchive->startRecording(
        m_recordingChannel, m_recordingStreamId, AeronArchive::SourceLocation::LOCAL);

    {
        std::shared_ptr<Publication> publication = addPublication(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);
        std::shared_ptr<Subscription> subscription = addSubscription(
            *aeronArchive->context().aeron(), m_recordingChannel, m_recordingStreamId);

        CountersReader& countersReader = aeronArchive->context().aeron()->countersReader();
        const std::int32_t counterId = getRecordingCounterId(publication->sessionId(), countersReader);
        const std::int64_t recordingId = RecordingPos::getRecordingId(countersReader, counterId);

        offerMessages(*publication, messageCount, messagePrefix);
        consumeMessages(*subscription, messageCount, messagePrefix);

        const std::int64_t currentPosition = publication->position();

        aeron::concurrent::YieldingIdleStrategy idle;
        while (countersReader.getCounterValue(counterId) < currentPosition)
        {
            idle.idle();
        }

        {
            std::shared_ptr<Subscription> replaySubscription = aeronArchive->replay(
                recordingId, currentPosition, NULL_LENGTH, m_replayChannel, m_replayStreamId);

            offerMessages(*publication, messageCount, messagePrefix);
            consumeMessages(*subscription, messageCount, messagePrefix);
            consumeMessages(*replaySubscription, messageCount, messagePrefix);

            const std::int64_t endPosition = publication->position();
            EXPECT_EQ(endPosition, replaySubscription->imageAtIndex(0).position());
        }
    }

    aeronArchive->stopRecording(subscriptionId);
}

struct SubscriptionDescriptor
{
    const std::int64_t m_controlSessionId;
    const std::int64_t m_correlationId;
    const std::int64_t m_subscriptionId;
    const std::int32_t m_streamId;
    const std::string m_strippedChannel;

    SubscriptionDescriptor(
        std::int64_t controlSessionId,
        std::int64_t correlationId,
        std::int64_t subscriptionId,
        std::int32_t streamId,
        const std::string& strippedChannel)
        :
        m_controlSessionId(controlSessionId),
        m_correlationId(correlationId),
        m_subscriptionId(subscriptionId),
        m_streamId(streamId),
        m_strippedChannel(strippedChannel)
    {
    }
};

TEST_F(AeronArchiveTest, shouldListRegisteredRecordingSubscriptions)
{
    std::vector<SubscriptionDescriptor> descriptors;
    recording_subscription_descriptor_consumer_t consumer = [&](
        std::int64_t controlSessionId,
        std::int64_t correlationId,
        std::int64_t subscriptionId,
        std::int32_t streamId,
        const std::string& strippedChannel)
    {
        descriptors.emplace_back(controlSessionId, correlationId, subscriptionId, streamId, strippedChannel);
    };

    const std::int32_t expectedStreamId = 7;
    const std::string channelOne = "aeron:ipc";
    const std::string channelTwo = "aeron:udp?endpoint=localhost:5678";
    const std::string channelThree = "aeron:udp?endpoint=localhost:4321";

    std::shared_ptr<AeronArchive> aeronArchive = AeronArchive::connect();

    const std::int64_t subIdOne = aeronArchive->startRecording(
        channelOne, expectedStreamId, AeronArchive::SourceLocation::LOCAL);
    const std::int64_t subIdTwo = aeronArchive->startRecording(
        channelTwo, expectedStreamId + 1, AeronArchive::SourceLocation::LOCAL);
    const std::int64_t subIdThree = aeronArchive->startRecording(
        channelThree, expectedStreamId + 2, AeronArchive::SourceLocation::LOCAL);

    const std::int32_t countOne = aeronArchive->listRecordingSubscriptions(
        0, 5, "ipc", expectedStreamId, true, consumer);

    EXPECT_EQ(1uL, descriptors.size());
    EXPECT_EQ(1L, countOne);

    descriptors.clear();

    const std::int32_t countTwo = aeronArchive->listRecordingSubscriptions(
        0, 5, "", expectedStreamId, false, consumer);

    EXPECT_EQ(3uL, descriptors.size());
    EXPECT_EQ(3L, countTwo);

    aeronArchive->stopRecording(subIdTwo);
    descriptors.clear();

    const std::int32_t countThree = aeronArchive->listRecordingSubscriptions(
        0, 5, "", expectedStreamId, false, consumer);

    EXPECT_EQ(2uL, descriptors.size());
    EXPECT_EQ(2L, countThree);

    EXPECT_EQ(1L, std::count_if(
        descriptors.begin(),
        descriptors.end(),
        [=](SubscriptionDescriptor s){ return s.m_subscriptionId == subIdOne;}));
    EXPECT_EQ(1L, std::count_if(
        descriptors.begin(),
        descriptors.end(),
        [=](SubscriptionDescriptor s){ return s.m_subscriptionId == subIdThree;}));
}
