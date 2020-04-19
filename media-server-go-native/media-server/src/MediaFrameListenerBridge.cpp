#include "video.h"
#include "audio.h"
#include "MediaFrameListenerBridge.h"
#include "VideoLayerSelector.h"

void MediaFrameListenerBridge::AddListener(RTPIncomingMediaStream::Listener* listener)
{
	Debug("-MediaFrameListenerBridge::AddListener() [listener:%p]\n",listener);
	ScopedLock scope(mutex);
	listeners.insert(listener);
}

void MediaFrameListenerBridge::RemoveListener(RTPIncomingMediaStream::Listener* listener)
{
	Debug("-MediaFrameListenerBridge::RemoveListener() [listener:%p]\n",listener);
	ScopedLock scope(mutex);
	listeners.erase(listener);
}

void MediaFrameListenerBridge::onMediaFrame(MediaFrame& frame)
{
	//Check
	if (!frame.HasRtpPacketizationInfo())
		//Error
		return;

	//If we need to reset
	if (reset)
	{
		//Reset first paquet seq num and timestamp
		firstTimestamp = 0;
		//Store the last send ones
		baseTimestamp = lastTimestamp;
		//Reseted
		reset = false;
	}
	
	//Get info
	const MediaFrame::RtpPacketizationInfo& info = frame.GetRtpPacketizationInfo();

	DWORD codec = 0;
	BYTE *frameData = NULL;
	DWORD frameSize = 0;
	WORD  rate = 1;

	//Depending on the type
	switch(frame.GetType())
	{
		case MediaFrame::Audio:
		{
			//get audio frame
			AudioFrame& audio = (AudioFrame&)frame;
			//Get codec
			codec = audio.GetCodec();
			//Get data
			frameData = audio.GetData();
			//Get size
			frameSize = audio.GetLength();
			//Set default rate
			rate = 48;
			break;
		}
		case MediaFrame::Video:
		{
			//get Video frame
			VideoFrame& video = (VideoFrame&)frame;
			//Get codec
			codec = video.GetCodec();
			//Get data
			frameData = video.GetData();
			//Get size
			frameSize = video.GetLength();
			//Set default rate
			rate = 90;
			break;
		}
		default:
			return;

	}
	
	//Get now
	auto now = getTimeMS();
	
	//Increase stats
	numFrames++;
	totalBytes += frameSize;
		
	//Sync
	{
		ScopedLock scope(mutex);
		//Update bitrate acumulator
		acumulator.Update(now,frameSize);
		//Get bitrate in bps
		bitrate = acumulator.GetInstant()*8;
	}
	

	//Check if it the first received packet
	if (!firstTimestamp)
	{
		//If we have a time offest from last sent packet
		if (lastTime)
			//Calculate time difd and add to the last sent timestamp
			baseTimestamp = lastTimestamp + getTimeDiff(lastTime)*rate/1000 + 1;
		//Get first timestamp
		firstTimestamp = frame.GetTimeStamp();
	}

	DWORD frameLength = 0;
	//Calculate total length
	for (size_t i=0;i<info.size();i++)
		//Get total length
		frameLength += info[i]->GetTotalLength();

	DWORD current = 0;

	//For each one
	for (size_t i=0;i<info.size();i++)
	{
		//Get packet
		MediaFrame::RtpPacketization* rtp = info[i];

		//Create rtp packet
		 auto packet = std::make_shared<RTPPacket>(frame.GetType(),codec);

		//Make sure it is enought length
		if (rtp->GetTotalLength()>packet->GetMaxMediaLength())
			//Error
			continue;
		//Set src
		packet->SetSSRC(ssrc);
		packet->SetExtSeqNum(extSeqNum++);
		//Set data
		packet->SetPayload(frameData+rtp->GetPos(),rtp->GetSize());
		//Add prefix
		packet->PrefixPayload(rtp->GetPrefixData(),rtp->GetPrefixLen());
		//Calculate timestamp
		lastTimestamp = baseTimestamp + (frame.GetTimeStamp()-firstTimestamp);
		//Set other values
		packet->SetTimestamp(lastTimestamp*rate);
		//Check
		if (i+1==info.size())
			//last
			packet->SetMark(true);
		else
			//No last
			packet->SetMark(false);
		//Calculate partial lenght
		current += rtp->GetPrefixLen()+rtp->GetSize();
		
		//Increase stats
		numPackets++;
		
		//Fill payload descriptors
		//TODO: move out of here
		if (frame.GetType()==MediaFrame::Video)
			VideoLayerSelector::GetLayerIds(packet);
		
		//Sync
		{
			ScopedLock scope(mutex);
			for (auto listener : listeners)
				listener->onRTP(this,packet);
		}
	}
}

void MediaFrameListenerBridge::Reset()
{
	reset = true;
}

void MediaFrameListenerBridge::Update()
{
	Update(getTimeMS());
}


void MediaFrameListenerBridge::Update(QWORD now)
{
	ScopedLock scope(mutex);
	//Update bitrate acumulator
	acumulator.Update(now);
	//Get bitrate in bps
	bitrate = acumulator.GetInstant()*8;
}
