/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */

#include <cstring> //memset

#include "OggStream.h"

#include "System/FileSystem/FileHandler.h"
#include "System/Sound/SoundLog.h"
#include "ALShared.h"
#include "VorbisShared.h"


namespace VorbisCallbacks {
	// NOTE:
	//   this buffer gets recycled by each new stream, across *all* audio-channels
	//   as a result streams are limited to only ever being played within a single
	//   channel (currently BGMusic), but cause far less memory fragmentation
	// TODO:
	//   can easily be fixed if necessary by giving each channel its own index and
	//   passing that along to the callbacks via COggStream{::Play}
	// CFileHandler fileBuffers[NUM_AUDIO_CHANNELS];
	CFileHandler fileBuffer("", "");

	size_t VorbisStreamReadCB(void* ptr, size_t size, size_t nmemb, void* datasource)
	{
		assert(datasource == &fileBuffer);
		return fileBuffer.Read(ptr, size * nmemb);
	}

	int VorbisStreamCloseCB(void* datasource)
	{
		assert(datasource == &fileBuffer);
		fileBuffer.Close();
		return 0;
	}

	int VorbisStreamSeekCB(void* datasource, ogg_int64_t offset, int whence)
	{
		assert(datasource == &fileBuffer);

		switch (whence) {
			case SEEK_SET: { fileBuffer.Seek(offset, std::ios_base::beg); } break;
			case SEEK_CUR: { fileBuffer.Seek(offset, std::ios_base::cur); } break;
			case SEEK_END: { fileBuffer.Seek(offset, std::ios_base::end); } break;
			default: {} break;
		}

		return 0;
	}

	long VorbisStreamTellCB(void* datasource)
	{
		assert(datasource == &fileBuffer);
		return (fileBuffer.GetPos());
	}
}



COggStream::COggStream(ALuint _source)
	: vorbisInfo(nullptr)
	, source(_source)
	, format(AL_FORMAT_MONO16)
	, stopped(true)
	, paused(false)
{
	memset(buffers, 0, NUM_BUFFERS * sizeof(buffers[0]));
	memset(pcmDecodeBuffer, 0, BUFFER_SIZE * sizeof(pcmDecodeBuffer[0]));
}


// open an Ogg stream from a given file and start playing it
void COggStream::Play(const std::string& path, float volume)
{
	// we're already playing another stream
	if (!stopped)
		return;

	vorbisTags.clear();

	ov_callbacks vorbisCallbacks;
	vorbisCallbacks.read_func  = VorbisCallbacks::VorbisStreamReadCB;
	vorbisCallbacks.close_func = VorbisCallbacks::VorbisStreamCloseCB;
	vorbisCallbacks.seek_func  = VorbisCallbacks::VorbisStreamSeekCB;
	vorbisCallbacks.tell_func  = VorbisCallbacks::VorbisStreamTellCB;

	VorbisCallbacks::fileBuffer.Open(path);

	const int result = ov_open_callbacks(&VorbisCallbacks::fileBuffer, &ovFile, nullptr, 0, vorbisCallbacks);

	if (result < 0) {
		LOG_L(L_WARNING, "Could not open Ogg stream (reason: %s).", ErrorString(result).c_str());
		VorbisCallbacks::fileBuffer.Close();
		return;
	}


	vorbisInfo = ov_info(&ovFile, -1);

	{
		vorbis_comment* vorbisComment = ov_comment(&ovFile, -1);
		vorbisTags.resize(vorbisComment->comments);

		for (unsigned i = 0; i < vorbisComment->comments; ++i) {
			vorbisTags[i] = std::string(vorbisComment->user_comments[i], vorbisComment->comment_lengths[i]);
		}

		vendor = std::string(vorbisComment->vendor);
		// DisplayInfo();
	}

	if (vorbisInfo->channels == 1) {
		format = AL_FORMAT_MONO16;
	} else {
		format = AL_FORMAT_STEREO16;
	}

	alGenBuffers(2, buffers); CheckError("COggStream::Play");

	if (!StartPlaying()) {
		ReleaseBuffers();
	} else {
		stopped = false;
		paused = false;
	}

	CheckError("COggStream::Play");
}

// stops the currently playing stream
void COggStream::Stop()
{
	if (stopped)
		return;

	ReleaseBuffers();

	msecsPlayed = spring_nulltime;
	lastTick = spring_gettime();

	source = 0;
	format = 0;
	vorbisInfo = nullptr;

	assert(!Valid());
}


float COggStream::GetTotalTime()
{
	return ov_time_total(&ovFile, -1);
}



// display Ogg info and comments
void COggStream::DisplayInfo()
{
	LOG("version:           %d", vorbisInfo->version);
	LOG("channels:          %d", vorbisInfo->channels);
	LOG("time (sec):        %lf", ov_time_total(&ovFile, -1));
	LOG("rate (Hz):         %ld", vorbisInfo->rate);
	LOG("bitrate (upper):   %ld", vorbisInfo->bitrate_upper);
	LOG("bitrate (nominal): %ld", vorbisInfo->bitrate_nominal);
	LOG("bitrate (lower):   %ld", vorbisInfo->bitrate_lower);
	LOG("bitrate (window):  %ld", vorbisInfo->bitrate_window);
	LOG("vendor:            %s", vendor.c_str());

	for (const std::string& s: vorbisTags) {
		LOG("%s", s.c_str());
	}
}


// clean up the OpenAL resources
void COggStream::ReleaseBuffers()
{
	stopped = true;
	paused = false;

	EmptyBuffers();

	alDeleteBuffers(2, buffers);
	CheckError("COggStream::ReleaseBuffers");

	ov_clear(&ovFile);
}


// returns true if both buffers were
// filled with data from the stream
bool COggStream::StartPlaying()
{
	msecsPlayed = spring_nulltime;
	lastTick = spring_gettime();

	if (!DecodeStream(buffers[0])) { return false; }
	if (!DecodeStream(buffers[1])) { return false; }

	alSourceQueueBuffers(source, 2, buffers); CheckError("COggStream::StartPlaying");
	alSourcePlay(source); CheckError("COggStream::StartPlaying");

	return true;
}


// returns true if we're still playing
bool COggStream::IsPlaying()
{
	ALenum state = 0;
	alGetSourcei(source, AL_SOURCE_STATE, &state);

	return (state == AL_PLAYING);
}

bool COggStream::TogglePause()
{
	if (!stopped)
		paused = !paused;

	return paused;
}


// pop the processed buffers from the queue,
// refill them, and push them back in line
bool COggStream::UpdateBuffers()
{
	int buffersProcessed = 0;
	bool active = true;

	alGetSourcei(source, AL_BUFFERS_PROCESSED, &buffersProcessed);

	while (buffersProcessed-- > 0) {
		ALuint buffer;
		alSourceUnqueueBuffers(source, 1, &buffer); CheckError("COggStream::UpdateBuffers");

		// false if we've reached end of stream
		active = DecodeStream(buffer);
		if (active) {
			alSourceQueueBuffers(source, 1, &buffer); CheckError("COggStream::UpdateBuffers");
		}
	}
	CheckError("COggStream::UpdateBuffers");

	return active;
}


void COggStream::Update()
{
	if (stopped)
		return;

	const spring_time tick = spring_gettime();

	if (!paused) {
		UpdateBuffers();

		if (!IsPlaying())
			ReleaseBuffers();

		msecsPlayed += (tick - lastTick);
	}

	lastTick = tick;
}


// read decoded data from audio stream into PCM buffer
bool COggStream::DecodeStream(ALuint buffer)
{
	memset(pcmDecodeBuffer, 0, BUFFER_SIZE);

	int size = 0;
	int section = 0;
	int result = 0;

	while (size < BUFFER_SIZE) {
		result = ov_read(&ovFile, pcmDecodeBuffer + size, BUFFER_SIZE - size, 0, 2, 1, &section);

		if (result > 0) {
			size += result;
			continue;
		}

		if (result < 0) {
			LOG_L(L_WARNING, "Error reading Ogg stream (%s)", ErrorString(result).c_str());
			continue;
		}

		break;
	}

	if (size == 0)
		return false;

	alBufferData(buffer, format, pcmDecodeBuffer, size, vorbisInfo->rate);
	CheckError("COggStream::DecodeStream");
	return true;
}


// dequeue any buffers pending on source
void COggStream::EmptyBuffers()
{
	int queuedBuffers = 0;
	alGetSourcei(source, AL_BUFFERS_QUEUED, &queuedBuffers); CheckError("COggStream::EmptyBuffers");

	while (queuedBuffers-- > 0) {
		ALuint buffer;
		alSourceUnqueueBuffers(source, 1, &buffer); CheckError("COggStream::EmptyBuffers");
	}
}

