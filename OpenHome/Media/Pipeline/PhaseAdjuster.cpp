#include <OpenHome/Media/Pipeline/PhaseAdjuster.h>
#include <OpenHome/Types.h>
#include <OpenHome/Av/SourceFactory.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Private/Debug.h>
#include <OpenHome/Media/Debug.h>
#include <OpenHome/Private/Standard.h>

#include <algorithm>

using namespace OpenHome;
using namespace OpenHome::Media;


// PhaseAdjuster

const TUint PhaseAdjuster::kSupportedMsgTypes =   eMode
                                                | eTrack
                                                | eDrain
                                                | eDelay
                                                | eEncodedStream
                                                | eAudioEncoded
                                                | eMetatext
                                                | eStreamInterrupted
                                                | eHalt
                                                | eFlush
                                                | eWait
                                                | eDecodedStream
                                                | eBitRate
                                                | eAudioPcm
                                                | eAudioDsd
                                                | eSilence
                                                | eQuit;

const TUint PhaseAdjuster::kDropLimitDelayOffsetJiffies;
const Brn PhaseAdjuster::kModeSongcast("Receiver"); // Av::SourceFactory::kSourceTypeReceiver

PhaseAdjuster::PhaseAdjuster(
    MsgFactory& aMsgFactory,
    IPipelineElementUpstream& aUpstreamElement,
    IStarvationRamper& aStarvationRamper,
    TUint aRampJiffiesLong,
    TUint aRampJiffiesShort,
    TUint aMinDelayJiffies
)
    : PipelineElement(kSupportedMsgTypes)
    , iMsgFactory(aMsgFactory)
    , iUpstreamElement(aUpstreamElement)
    , iStarvationRamper(aStarvationRamper)
    , iAnimator(nullptr)
    , iEnabled(false)
    , iState(State::Running)
    , iLock("SPAL")
    , iUpdateCount(0)
    , iTrackedJiffies(0)
    , iAudioIn(0)
    , iAudioOut(0)
    , iDecodedStream(nullptr)
    , iDelayJiffies(0)
    , iDelayTotalJiffies(0)
    , iDroppedJiffies(0)
    , iRampJiffiesLong(aRampJiffiesLong)
    , iRampJiffiesShort(aRampJiffiesShort)
    , iMinDelayJiffies(aMinDelayJiffies)
    , iRampJiffies(iRampJiffiesLong)
    , iRemainingRampSize(0)
    , iCurrentRampValue(Ramp::kMin)
    , iConfirmOccupancy(false)
{
}

PhaseAdjuster::~PhaseAdjuster()
{
    iQueue.Clear();
    ClearDecodedStream();
}

void PhaseAdjuster::SetAnimator(IPipelineAnimator& aAnimator)
{
    iAnimator = &aAnimator;
}

Msg* PhaseAdjuster::Pull()
{
    Msg* msg = nullptr;
    do {
        if (!iQueue.IsEmpty()) {
            msg = iQueue.Dequeue();
        }
        else {
            msg = iUpstreamElement.Pull();
            msg = msg->Process(*this);
            if (iConfirmOccupancy) {
                iStarvationRamper.WaitForOccupancy(iAnimator->PipelineAnimatorBufferJiffies());
                iConfirmOccupancy = false;
            }
        }
    } while (msg == nullptr);
    return msg;
}

Msg* PhaseAdjuster::ProcessMsg(MsgMode* aMsg)
{
    if (aMsg->Info().SupportsLatency()) {
        iEnabled = true;
        iRampJiffies = aMsg->Info().RampPauseResumeLong()?
                        iRampJiffiesLong : iRampJiffiesShort;
        iDelayJiffies = iDelayTotalJiffies = 0;
        ResetPhaseDelay();
    }
    else {
        iEnabled = false;
        iState = State::Running;
    }
    return aMsg;
}

Msg* PhaseAdjuster::ProcessMsg(MsgDrain* aMsg)
{
    // MsgDrain is generated by element to left of this whenever MsgHalt seen (i.e., no need for this class to also reset on MsgHalt).
    if (iEnabled) {
        ResetPhaseDelay();
    }

    return aMsg;
}

Msg* PhaseAdjuster::ProcessMsg(MsgDelay* aMsg)
{
    if (iEnabled) {
        iDelayTotalJiffies = aMsg->TotalJiffies();
        TryCalculateDelay();
    }
    aMsg->RemoveRef();
    return nullptr;
}

Msg* PhaseAdjuster::ProcessMsg(MsgDecodedStream* aMsg)
{
    ClearDecodedStream();
    if (iEnabled) {
        aMsg->AddRef();
        iDecodedStream = aMsg;
        TryCalculateDelay();
    }
    return aMsg;
}

Msg* PhaseAdjuster::ProcessMsg(MsgAudioPcm* aMsg)
{
    if (iEnabled) {
        return AdjustAudio(Brn("audio"), aMsg);
    }
    return aMsg;
}

Msg* PhaseAdjuster::ProcessMsg(MsgSilence* aMsg)
{
    // Delay will increase and/or gain accuracy the more silence is allowed to pass through the pipeline.
    // Therefore, easiest to allow all MsgSilence to pass to get a snapshot of delay when first MsgAudio seen, and only drop from the start of MsgAudio.
    // Otherwise, if start dropping too early in MsgSilence, can end up dropping so many MsgSilence that we don't get a reasonable estimate of accumulated error and quickly bring the error close to 0 and stop dropping early on in MsgAudio.

    return aMsg;
}

void PhaseAdjuster::Update(TInt aDelta)
{
    iTrackedJiffies += aDelta;
    iUpdateCount++;

    if (aDelta < 0) {
        iAudioOut -= aDelta;
    }
    else {
        iAudioIn += aDelta;
    }
}

void PhaseAdjuster::Start()
{
}

void PhaseAdjuster::Stop()
{
}

void PhaseAdjuster::TryCalculateDelay()
{
    iDelayJiffies = 0;
    if (iDecodedStream == nullptr) {
        return;
    }
    if (iDelayTotalJiffies == 0) {
        return;
    }
    ASSERT(iAnimator != nullptr);
    const auto& stream = iDecodedStream->StreamInfo();
    const auto animatorDelayJiffies = iAnimator->PipelineAnimatorDelayJiffies(
        stream.Format(), stream.SampleRate(), stream.BitDepth(), stream.NumChannels());
    if (iDelayTotalJiffies > animatorDelayJiffies) {
        iDelayJiffies = iDelayTotalJiffies - animatorDelayJiffies;
        iDelayJiffies = std::max(iDelayJiffies, iMinDelayJiffies);
    }
}

MsgAudio* PhaseAdjuster::AdjustAudio(const Brx& /*aMsgType*/, MsgAudio* aMsg)
{
    if (iState == State::Starting) {
        // log intention to discard audio once (function may be called many times with
        // State::Adjusting while we discard and we can't afford to log all these times).
        const TUint trackedJiffies = static_cast<TUint>(iTrackedJiffies);
        LOG(kPipeline, "PhaseAdjuster: tracked=%u (%ums), delay=%u (%ums)\n",
                       trackedJiffies, Jiffies::ToMs(trackedJiffies), iDelayJiffies, Jiffies::ToMs(iDelayJiffies));
        iState = State::Adjusting;
    }

    if (iState == State::Running) {
        return aMsg;
    }
    else if (iState == State::RampingUp) {
        return RampUp(aMsg);
    }
    else { // iState == State::Adjusting
        if (iDelayJiffies == 0) {
            // No MsgDelay (with value > 0) was seen. Switch to running state.
            iState = State::Running;
            return aMsg;
        }
        TInt error = iTrackedJiffies - iDelayJiffies;
        if (error > 0) {
            // Drop audio.
            TUint dropped = 0;
            MsgAudio* msg = DropAudio(aMsg, error, dropped);
            iStarvationRamper.WaitForOccupancy(iAnimator->PipelineAnimatorBufferJiffies());
            iDroppedJiffies += dropped;
            error -= dropped;
            if (msg != nullptr) {
                // Have dropped audio so must now ramp up.
                return StartRampUp(msg);
            }
            return msg;
        }
        else if (error < 0) {
            // Error is 0 or receiver is in front of sender. Highly unlikely receiver would get in front of sender. Any error would likely be minimal. Do nothing.
            // If error < 0, could inject MsgSilence to pull the error in towards 0.
            LOG(kPipeline, "PhaseAdjuster: latency is now too low (error=%d)\n", error);
            iState = State::Running;
            return aMsg;
        }
        else { // error == 0
            if (iDroppedJiffies > 0) {
                return StartRampUp(aMsg);
            }
            else {
                LOG(kPipeline, "PhaseAdjuster: completed adjustment, dropped 0 jiffies\n");
                iState = State::Running;
            }
            return aMsg;
        }
    }
}

MsgAudio* PhaseAdjuster::DropAudio(MsgAudio* aMsg, TUint aJiffies, TUint& aDroppedJiffies)
{
    ASSERT(aMsg != nullptr);
    if (aJiffies >= aMsg->Jiffies()) {
        aDroppedJiffies = aMsg->Jiffies();
        aMsg->RemoveRef();
        return nullptr;
    }
    else if (aJiffies < aMsg->Jiffies()) {
        auto* remaining = aMsg->Split(aJiffies);
        aDroppedJiffies = aJiffies;
        aMsg->RemoveRef();
        return remaining;
    }
    // aJiffies == 0.
    aDroppedJiffies = 0;
    return aMsg;
}

MsgSilence* PhaseAdjuster::InjectSilence(TUint aJiffies)
{
    ASSERT(iDecodedStream != nullptr);
    const auto& stream = iDecodedStream->StreamInfo();
    TUint jiffies = aJiffies;
    auto* msg = iMsgFactory.CreateMsgSilence(jiffies, stream.SampleRate(), stream.BitDepth(), stream.NumChannels());
    iInjectedJiffies += jiffies;
    return msg;
}

MsgAudio* PhaseAdjuster::RampUp(MsgAudio* aMsg)
{
    ASSERT(aMsg != nullptr);
    MsgAudio* split;
    if (aMsg->Jiffies() > iRemainingRampSize && iRemainingRampSize > 0) {
        split = aMsg->Split(iRemainingRampSize);
        if (split != nullptr) {
            iQueue.Enqueue(split);
        }
    }
    split = nullptr;
    if (iRemainingRampSize > 0) {
        iCurrentRampValue = aMsg->SetRamp(iCurrentRampValue, iRemainingRampSize, Ramp::EUp, split);
    }
    if (split != nullptr) {
        iQueue.Enqueue(split);
    }
    if (iRemainingRampSize == 0) {
        iState = State::Running;
    }
    return aMsg;
}

MsgAudio* PhaseAdjuster::StartRampUp(MsgAudio* aMsg)
{
    LOG(kPipeline, "PhaseAdjuster::StartRampUp dropped %u jiffies (%ums)\n",
                   iDroppedJiffies, Jiffies::ToMs(iDroppedJiffies));
    iState = State::RampingUp;
    iRemainingRampSize = iRampJiffies;
    iConfirmOccupancy = true; /* We've discarded some audio.  There may now be no audio in
                                 StarvationRamper but plenty upstream of it.  Instruct
                                 StarvationRamper to wait for some of this audio to reach
                                 it before we try Pull()ing.  (Since audio is already in
                                 the pipeline, this will take a tiny amount of time if we
                                 yield from our higher priority thread.) */

    ASSERT(iDecodedStream != nullptr);
    const auto& info = iDecodedStream->StreamInfo();
    const TUint droppedSamples = iDroppedJiffies / Jiffies::PerSample(info.SampleRate());
    const TUint64 sampleStart = info.SampleStart() + droppedSamples;
    auto* msgDecodedStream = iMsgFactory.CreateMsgDecodedStream(
        info.StreamId(),
        info.BitRate(),
        info.BitDepth(),
        info.SampleRate(),
        info.NumChannels(),
        info.CodecName(),
        info.TrackLength(),
        sampleStart,
        info.Lossless(),
        info.Seekable(),
        info.Live(),
        info.AnalogBypass(),
        info.Format(),
        info.Multiroom(),
        info.Profile(),
        info.StreamHandler(),
        info.Ramp()
    );

    // No way to pass MsgDecodedStream up through chain of calls that get here.
    // So, queue up MsgDecodedStream and MsgAudio.
    iQueue.Enqueue(msgDecodedStream);

    ClearDecodedStream();
    msgDecodedStream->AddRef();
    iDecodedStream = msgDecodedStream;

    if (aMsg != nullptr) {
        auto* msgAudio = RampUp(aMsg);
        // MsgDecodedStream has been queued up, so must queue audio up behind it rather than returning from here.
        iQueue.Enqueue(msgAudio);
    }
    return nullptr;
}

void PhaseAdjuster::ResetPhaseDelay()
{
    iState = State::Starting;

    iDroppedJiffies = 0;
    iInjectedJiffies = 0;

    iRemainingRampSize = iRampJiffies;
    iCurrentRampValue = Ramp::kMin;
}

void PhaseAdjuster::ClearDecodedStream()
{
    if (iDecodedStream != nullptr) {
        iDecodedStream->RemoveRef();
        iDecodedStream = nullptr;
    }
}

void PhaseAdjuster::PrintStats(const Brx& /*aMsgType*/, TUint /*aJiffies*/)
{
    // static const TUint kInitialJiffiesTrackingLimit = 50 * Jiffies::kPerMs;
    // static const TUint kJiffiesStatsInterval = 50 * Jiffies::kPerMs;
    // static const TUint kJiffiesStatsLimit = 500 * Jiffies::kPerMs;
    // if ((aJiffies < kInitialJiffiesTrackingLimit || aJiffies % kJiffiesStatsInterval == 0) && aJiffies <= kJiffiesStatsLimit) {
    //     const TInt tj = iTrackedJiffies;
    //     const TInt err = tj - iDelayJiffies;
    //     const TUint in = iAudioIn;
    //     const TUint out = iAudioOut;
    //
    //     Log::Print("PhaseAdjuster::PrintStats aMsgType: %.*s, aJiffies: %u (%u ms), tracked jiffies: %d (%u ms), err: %d (%u ms), in: %u (%u ms), out: %u (%u ms)\n", PBUF(aMsgType), aJiffies, Jiffies::ToMs(aJiffies), tj, Jiffies::ToMs((TUint)tj), err, Jiffies::ToMs((TUint)err), in, Jiffies::ToMs(in), out, Jiffies::ToMs(out));
    // }
}
