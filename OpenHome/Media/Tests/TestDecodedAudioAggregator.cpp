#ifndef HEADER_TESTCODECCONTROLLER
#define HEADER_TESTCODECCONTROLLER

#include <OpenHome/Private/SuiteUnitTest.h>
#include <OpenHome/Media/Pipeline/DecodedAudioAggregator.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/InfoProvider.h>
#include <OpenHome/Media/Utils/AllocatorInfoLogger.h>
#include <OpenHome/Private/Arch.h>
#include <OpenHome/Media/Utils/ProcessorPcmUtils.h>

#include <list>
#include <limits.h>

using namespace OpenHome;
using namespace OpenHome::TestFramework;
using namespace OpenHome::Media;

namespace OpenHome {
namespace Media {

class SuiteDecodedAudioAggregator : public SuiteUnitTest
                                  , public IPipelineElementDownstream
                                  , public IStreamHandler
                                  , public IMsgProcessor
{
public:
    SuiteDecodedAudioAggregator();
private: // from SuiteUnitTest
    void Setup();
    void TearDown();
private: // from IPipelineElementDownstream
    void Push(Msg* aMsg);
private: // from IStreamHandler
    EStreamPlay OkToPlay(TUint aTrackId, TUint aStreamId);
    TUint TrySeek(TUint aTrackId, TUint aStreamId, TUint64 aOffset);
    TUint TryStop(TUint aTrackId, TUint aStreamId);
    TBool TryGet(IWriter& aWriter, TUint aTrackId, TUint aStreamId, TUint64 aOffset, TUint aBytes);
    void NotifyStarving(const Brx& aMode, TUint aTrackId, TUint aStreamId);
private: // from IMsgProcessor
    Msg* ProcessMsg(MsgMode* aMsg);
    Msg* ProcessMsg(MsgSession* aMsg);
    Msg* ProcessMsg(MsgTrack* aMsg);
    Msg* ProcessMsg(MsgDelay* aMsg);
    Msg* ProcessMsg(MsgEncodedStream* aMsg);
    Msg* ProcessMsg(MsgAudioEncoded* aMsg);
    Msg* ProcessMsg(MsgMetaText* aMsg);
    Msg* ProcessMsg(MsgHalt* aMsg);
    Msg* ProcessMsg(MsgFlush* aMsg);
    Msg* ProcessMsg(MsgWait* aMsg);
    Msg* ProcessMsg(MsgDecodedStream* aMsg);
    Msg* ProcessMsg(MsgAudioPcm* aMsg);
    Msg* ProcessMsg(MsgSilence* aMsg);
    Msg* ProcessMsg(MsgPlayable* aMsg);
    Msg* ProcessMsg(MsgQuit* aMsg);
protected:
    enum EMsgType
    {
        ENone
       ,EMsgMode
       ,EMsgSession
       ,EMsgTrack
       ,EMsgDelay
       ,EMsgEncodedStream
       ,EMsgMetaText
       ,EMsgDecodedStream
       ,EMsgAudioPcm
       ,EMsgSilence
       ,EMsgHalt
       ,EMsgFlush
       ,EMsgWait
       ,EMsgQuit
    };
private:
    void Queue(Msg* aMsg);
    void PullNext(EMsgType aExpectedMsg);
    void PullNext(EMsgType aExpectedMsg, TUint64 aExpectedJiffies);
    Msg* CreateTrack();
    Msg* CreateEncodedStream();
    MsgDecodedStream* CreateDecodedStream();
    MsgFlush* CreateFlush();
    MsgAudioPcm* CreateAudio(TUint aBytes);
private:
    void TestStreamSuccessful();
    void TestNoDataAfterDecodedStream();
    void TestShortStream();
    void TestTrackTrack();
    void TestTrackEncodedStreamTrack();
    void TestPcmIsExpectedSize();
private:
    static const TUint kWavHeaderBytes = 44;
    static const TUint kSampleRate = 44100;
    static const TUint kChannels = 2;
    static const TUint kBitDepth = 16;
    static const TUint kExpectedFlushId = 5;
    static const TUint kSemWaitMs = 5000;   // only required in case tests fail
    MsgFactory* iMsgFactory;
    DecodedAudioAggregator* iDecodedAudioAggregator;
    Semaphore* iSemStop;
    TUint64 iTrackOffset;
    TUint iTrackOffsetBytes;
    TUint64 iJiffies;
    TUint64 iMsgOffset;
    TUint iStopCount;
    TUint iTrackId;
    TUint iStreamId;

    AllocatorInfoLogger iInfoAggregator;
    TrackFactory* iTrackFactory;
    std::list<Msg*> iReceivedMsgs;
    Semaphore* iSemReceived;
    Mutex* iLockReceived;
    EMsgType iLastReceivedMsg;
    TUint iNextTrackId;
    TUint iNextStreamId;
    TBool iSeekable;
};

} // namespace Media
} // namespace OpenHome



// SuiteDecodedAudioAggregator

SuiteDecodedAudioAggregator::SuiteDecodedAudioAggregator()
    : SuiteUnitTest("SuiteDecodedAudioAggregator")
{
    AddTest(MakeFunctor(*this, &SuiteDecodedAudioAggregator::TestStreamSuccessful), "TestStreamSuccessful");
    AddTest(MakeFunctor(*this, &SuiteDecodedAudioAggregator::TestNoDataAfterDecodedStream), "TestNoDataAfterDecodedStream");
    AddTest(MakeFunctor(*this, &SuiteDecodedAudioAggregator::TestShortStream), "TestShortStream");
    AddTest(MakeFunctor(*this, &SuiteDecodedAudioAggregator::TestTrackTrack), "TestTrackTrack");
    AddTest(MakeFunctor(*this, &SuiteDecodedAudioAggregator::TestTrackEncodedStreamTrack), "TestTrackEncodedStreamTrack");
    AddTest(MakeFunctor(*this, &SuiteDecodedAudioAggregator::TestPcmIsExpectedSize), "TestPcmIsExpectedSize");
}

void SuiteDecodedAudioAggregator::Setup()
{
    iTrackFactory = new TrackFactory(iInfoAggregator, 5);
    // Need so many (Msg)AudioEncoded because kMaxMsgBytes is currently 960, and msgs are queued in advance of being pulled for these tests.
    iMsgFactory = new MsgFactory(iInfoAggregator, 400, 400, 100, 100, 10, 50, 0, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1);
    iDecodedAudioAggregator = new DecodedAudioAggregator(*this, *iMsgFactory);
    iSemReceived = new Semaphore("TCSR", 0);
    iSemStop = new Semaphore("TCSS", 0);
    iLockReceived = new Mutex("TCMR");
    iTrackId = iStreamId = UINT_MAX;
    iNextTrackId = iNextStreamId = 0;
    iTrackOffsetBytes = 0;
    iTrackOffset = 0;
    iJiffies = 0;
    iMsgOffset = 0;
    iSeekable = true;
    iStopCount = 0;
}

void SuiteDecodedAudioAggregator::TearDown()
{
    Queue(iMsgFactory->CreateMsgQuit());
    PullNext(EMsgQuit);

    iLockReceived->Wait();
    ASSERT(iReceivedMsgs.size() == 0);
    iLockReceived->Signal();

    delete iLockReceived;
    delete iSemStop;
    delete iSemReceived;
    delete iDecodedAudioAggregator;
    delete iMsgFactory;
    delete iTrackFactory;
}

void SuiteDecodedAudioAggregator::Push(Msg* aMsg)
{
    iLockReceived->Wait();
    iReceivedMsgs.push_back(aMsg);
    iLockReceived->Signal();
    iSemReceived->Signal();
}

EStreamPlay SuiteDecodedAudioAggregator::OkToPlay(TUint /*aTrackId*/, TUint /*aStreamId*/)
{
    ASSERTS();
    return ePlayNo;
}

TUint SuiteDecodedAudioAggregator::TrySeek(TUint /*aTrackId*/, TUint /*aStreamId*/, TUint64 /*aOffset*/)
{
    ASSERTS();
    return MsgFlush::kIdInvalid;
}

TUint SuiteDecodedAudioAggregator::TryStop(TUint aTrackId, TUint aStreamId)
{
    iStopCount++;
    iSemStop->Signal();
    if (aTrackId == iTrackId && aStreamId == iStreamId) {
        return kExpectedFlushId;
    }
    ASSERTS();
    return MsgFlush::kIdInvalid;
}

TBool SuiteDecodedAudioAggregator::TryGet(IWriter& /*aWriter*/, TUint /*aTrackId*/, TUint /*aStreamId*/, TUint64 /*aOffset*/, TUint /*aBytes*/)
{
    ASSERTS();
    return false;
}

void SuiteDecodedAudioAggregator::NotifyStarving(const Brx& /*aMode*/, TUint /*aTrackId*/, TUint /*aStreamId*/)
{
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgMode* aMsg)
{
    iLastReceivedMsg = EMsgMode;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgSession* aMsg)
{
    iLastReceivedMsg = EMsgSession;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgTrack* aMsg)
{
    iLastReceivedMsg = EMsgTrack;
    iTrackId = aMsg->IdPipeline();
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgDelay* aMsg)
{
    iLastReceivedMsg = EMsgDelay;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgEncodedStream* aMsg)
{
    iLastReceivedMsg = EMsgEncodedStream;
    iStreamId = aMsg->StreamId();
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgAudioEncoded* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgMetaText* aMsg)
{
    iLastReceivedMsg = EMsgMetaText;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgHalt* aMsg)
{
    iLastReceivedMsg = EMsgHalt;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgFlush* aMsg)
{
    iLastReceivedMsg = EMsgFlush;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgWait* aMsg)
{
    iLastReceivedMsg = EMsgWait;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgDecodedStream* aMsg)
{
    iLastReceivedMsg = EMsgDecodedStream;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgAudioPcm* aMsg)
{
    iLastReceivedMsg = EMsgAudioPcm;
    iMsgOffset = aMsg->TrackOffset();
    iJiffies += aMsg->Jiffies();
    MsgPlayable* playable = aMsg->CreatePlayable();
    ProcessorPcmBufPacked pcmProcessor;
    playable->Read(pcmProcessor);
    Brn buf(pcmProcessor.Buf());
    ASSERT(buf.Bytes() >= 4);   // check we have enough bytes to examine first
                                // and last subsamples before manipulating pointers
    const TByte* ptr = buf.Ptr();
    const TUint bytes = buf.Bytes();
    const TUint firstSubsample = (ptr[0]<<8) | ptr[1];
    TEST(firstSubsample == 0x7f7f);
    const TUint lastSubsample = (ptr[bytes-2]<<8) | ptr[bytes-1];
    TEST(lastSubsample == 0x7f7f);

    return playable;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgSilence* aMsg)
{
    iLastReceivedMsg = EMsgSilence;
    return aMsg;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgPlayable* /*aMsg*/)
{
    ASSERTS();
    return NULL;
}

Msg* SuiteDecodedAudioAggregator::ProcessMsg(MsgQuit* aMsg)
{
    iLastReceivedMsg = EMsgQuit;
    return aMsg;
}

void SuiteDecodedAudioAggregator::Queue(Msg* aMsg)
{
    iDecodedAudioAggregator->Push(aMsg);
}

void SuiteDecodedAudioAggregator::PullNext(EMsgType aExpectedMsg)
{
    iSemReceived->Wait(kSemWaitMs);
    iLockReceived->Wait();
    ASSERT(iReceivedMsgs.size() > 0);
    Msg* msg = iReceivedMsgs.front();
    iReceivedMsgs.pop_front();
    iLockReceived->Signal();

    msg = msg->Process(*this);
    msg->RemoveRef();
    TEST(iLastReceivedMsg == aExpectedMsg);
}

void SuiteDecodedAudioAggregator::PullNext(EMsgType aExpectedMsg, TUint64 aExpectedJiffies)
{
    TUint64 jiffiesStart = iJiffies;
    PullNext(aExpectedMsg);
    TUint64 jiffiesDiff = iJiffies - jiffiesStart;
    //Log::Print("jiffiesDiff: %llu, aExpectedJiffies: %llu\n", jiffiesDiff, aExpectedJiffies);
    TEST(jiffiesDiff == aExpectedJiffies);
}

Msg* SuiteDecodedAudioAggregator::CreateTrack()
{
    Track* track = iTrackFactory->CreateTrack(Brx::Empty(), Brx::Empty());
    Msg* msg = iMsgFactory->CreateMsgTrack(*track, ++iNextTrackId);
    track->RemoveRef();
    return msg;
}

Msg* SuiteDecodedAudioAggregator::CreateEncodedStream()
{
    return iMsgFactory->CreateMsgEncodedStream(Brx::Empty(), Brx::Empty(), 1<<21, ++iNextStreamId, iSeekable, false, this);
}

MsgDecodedStream* SuiteDecodedAudioAggregator::CreateDecodedStream()
{
    static const TUint kBitrate = 256;
    return iMsgFactory->CreateMsgDecodedStream(++iNextStreamId, kBitrate, kBitDepth, kSampleRate, kChannels, Brn("NULL"), 0, 0, true, true, false, this);
}

MsgFlush* SuiteDecodedAudioAggregator::CreateFlush()
{
    return iMsgFactory->CreateMsgFlush(kExpectedFlushId);
}

MsgAudioPcm* SuiteDecodedAudioAggregator::CreateAudio(TUint aBytes)
{
    static const TUint kByteDepth = kBitDepth/8;

    TByte* decodedAudioData = new TByte[aBytes];
    (void)memset(decodedAudioData, 0x7f, aBytes);
    Brn decodedAudioBuf(decodedAudioData, aBytes);
    MsgAudioPcm* audio = iMsgFactory->CreateMsgAudioPcm(decodedAudioBuf, kChannels, kSampleRate, kBitDepth, EMediaDataLittleEndian, iTrackOffset);
    delete decodedAudioData;

    TUint samples = aBytes / (kChannels*kByteDepth);
    TUint jiffiesPerSample = Jiffies::kPerSecond / kSampleRate;
    iTrackOffset += samples * jiffiesPerSample;
    iTrackOffsetBytes += aBytes;
    return audio;
}

void SuiteDecodedAudioAggregator::TestStreamSuccessful()
{
    static const TUint kMaxMsgBytes = DecodedAudio::kMaxBytes;
    static const TUint kAudioBytes = DecodedAudio::kMaxBytes*5;
    Queue(CreateTrack());
    PullNext(EMsgTrack);
    Queue(CreateEncodedStream());
    PullNext(EMsgEncodedStream);
    Queue(CreateDecodedStream());
    PullNext(EMsgDecodedStream);

    while (iTrackOffsetBytes < kAudioBytes) {
        Queue(CreateAudio(kMaxMsgBytes));
    }
    PullNext(EMsgAudioPcm);
    PullNext(EMsgAudioPcm);
    PullNext(EMsgAudioPcm);
    PullNext(EMsgAudioPcm);
    PullNext(EMsgAudioPcm);

    ASSERT(iTrackOffsetBytes == kAudioBytes);
    TEST(iJiffies == iTrackOffset);
}

void SuiteDecodedAudioAggregator::TestNoDataAfterDecodedStream()
{
    Queue(CreateTrack());
    PullNext(EMsgTrack);
    Queue(CreateEncodedStream());
    PullNext(EMsgEncodedStream);
    Queue(CreateDecodedStream());
    PullNext(EMsgDecodedStream);

    // Send a new MsgTrack.
    Queue(CreateTrack());
    PullNext(EMsgTrack);

    TEST(iJiffies == iTrackOffset);
}

void SuiteDecodedAudioAggregator::TestShortStream()
{
    static const TUint kMaxMsgBytes = DecodedAudio::kMaxBytes;
    Queue(CreateTrack());
    PullNext(EMsgTrack);
    Queue(CreateEncodedStream());
    PullNext(EMsgEncodedStream);
    Queue(CreateDecodedStream());
    PullNext(EMsgDecodedStream);

    Queue(CreateAudio(kMaxMsgBytes));
    PullNext(EMsgAudioPcm);

    ASSERT(iTrackOffsetBytes == kMaxMsgBytes);
    TEST(iJiffies == iTrackOffset);
}

void SuiteDecodedAudioAggregator::TestTrackTrack()
{
    Queue(CreateTrack());
    PullNext(EMsgTrack);

    Queue(CreateTrack());
    PullNext(EMsgTrack);
}

void SuiteDecodedAudioAggregator::TestTrackEncodedStreamTrack()
{
    Queue(CreateTrack());
    PullNext(EMsgTrack);

    Queue(CreateEncodedStream());
    PullNext(EMsgEncodedStream);

    Queue(CreateTrack());
    PullNext(EMsgTrack);
}

void SuiteDecodedAudioAggregator::TestPcmIsExpectedSize()
{
    static const TUint kMaxMsgBytes = 64;
    static const TUint kAudioBytes = DecodedAudio::kMaxBytes;
    static const TUint kSamplesPerMsg = 16;
    static const TUint64 kJiffiesPerMsg = (Jiffies::kPerSecond / kSampleRate) * kSamplesPerMsg;
    static const TUint kMaxDecodedBufferedMs = 5;   // dependant on value in CodecController
    static const TUint kMaxDecodedBufferedJiffies = Jiffies::kPerMs * kMaxDecodedBufferedMs;

    // kMaxDecodedBufferedJiffies probably isn't on a msg boundary, so:
    // - work out how much over last boundary it goes
    static const TUint kRemainderJiffies = kMaxDecodedBufferedJiffies % kJiffiesPerMsg;
    // - and then adjust expected jiffies to last boundary + 1 msg (as ms limit can be violated as long as byte limit is not violated)
    static const TUint kExpectedJiffiesPerMsg = kMaxDecodedBufferedJiffies-kRemainderJiffies + kJiffiesPerMsg;

    ASSERT(kMaxDecodedBufferedJiffies%kJiffiesPerMsg != 0);

    Queue(CreateTrack());
    PullNext(EMsgTrack);
    Queue(CreateEncodedStream());
    PullNext(EMsgEncodedStream);
    Queue(CreateDecodedStream());
    PullNext(EMsgDecodedStream);

    while (iTrackOffsetBytes < kAudioBytes) {
        Queue(CreateAudio(kMaxMsgBytes));
    }
    Queue(CreateEncodedStream());   // flush out remaining audio

    while (iJiffies < (iTrackOffset-kMaxDecodedBufferedJiffies)) {
        PullNext(EMsgAudioPcm, kExpectedJiffiesPerMsg);
    }
    if (iJiffies < iTrackOffset) {
        // Still a final (shorter) MsgAudioPcm to pull.
        const TUint64 finalMsgJiffies = iTrackOffset-iJiffies;
        PullNext(EMsgAudioPcm, finalMsgJiffies);
    }

    PullNext(EMsgEncodedStream);

    ASSERT(iTrackOffsetBytes == kAudioBytes); // check correct number of bytes have been output by test code
    TEST(iJiffies == iTrackOffset);
}


void TestDecodedAudioAggregator()
{
    Runner runner("CodecController tests\n");
    runner.Add(new SuiteDecodedAudioAggregator());
    runner.Run();
}

#endif // HEADER_TESTCODECCONTROLLER