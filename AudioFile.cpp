#include "AudioFile.h"
#include <unistd.h>
#include <string.h>

std::vector<float>& AudioFile::getRtBuffer()
{
	size_t idx;
	if(ramOnly)
		idx = 0;
	else
		idx = !ioBuffer;
	return internalBuffers[idx];
}

// socket code from https://riptutorial.com/cplusplus/example/24000/hello-tcp-client
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
struct sf_socket_t {
	int sock;
	unsigned int count;
};

static inline sf_socket_t* to_sf_socket(void* user_data)
{
	return static_cast<sf_socket_t*>(user_data);
}

static sf_count_t sf_socket_get_filelen(void *user_data)
{
	printf("SOCKET_GET_FILELEN\n");
	return 0;
	sf_socket_t* sf_socket = to_sf_socket(user_data);
	return sf_socket->count;
}

static sf_count_t sf_socket_seek(sf_count_t offset, int whence, void *user_data)
{
	return ESPIPE;
}

static sf_count_t sf_socket_read(void *ptr, sf_count_t count, void *user_data)
{
	printf("SOCKET_READ\n");
	sf_socket_t* sf_socket = to_sf_socket(user_data);
	int ret = recv(sf_socket->sock, ptr, count, 0);
	if(ret >= 0)
		sf_socket->count += ret;
	return ret;
}

static sf_count_t sf_socket_write (const void *ptr, sf_count_t count, void *user_data)
{
	sf_socket_t* sf_socket = to_sf_socket(user_data);
	int ret = write(sf_socket->sock, ptr, count);
	//printf("W %d\n", ret);
	if(ret >= 0)
		sf_socket->count += ret;
	return ret;
}

static sf_count_t sf_socket_tell(void *user_data)
{
	printf("SOCKET_TELL\n");
	sf_socket_t* sf_socket = to_sf_socket(user_data);
	return sf_socket->count;
}

static int sf_socket_setup(sf_socket_t* data, const char* ipAddress, const char* portNum)
{
	if(!data)
		return 1;
	data->count = 0;
	addrinfo hints, *p;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags	= AI_PASSIVE;

	int gAddRes = getaddrinfo(ipAddress, portNum, &hints, &p);
	if (gAddRes != 0) {
		fprintf(stderr, "%s\n", gai_strerror(gAddRes));
		return 1;
	}

	if (p == NULL) {
		fprintf(stderr, "No addresses found\n");
		return 1;
	}

	// socket() call creates a new socket and returns its descriptor
	int sockFD = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
	if (sockFD < 0) {
		fprintf(stderr, "Error while creating socket\n");
		return 1;
	}

	// connect() call tries to establish a TCP connection to the specified server
	int connectR = connect(sockFD, p->ai_addr, p->ai_addrlen);
	if (connectR == -1) {
		close(sockFD);
		fprintf(stderr, "Error while connecting socket\n");
		return 1;
	}
	data->sock = sockFD;
	return 0;
}

sf_socket_t mysock;

#include <iostream>
#include <fstream>
#include "/root/libsndfile/include/sndfile.h"

void print_info(SF_INFO * sfinfo) {
	std::cout << "---------/// INFO ///---------" << std::endl;
	std::cout << "Frames     " << (*sfinfo).frames << std::endl;
	std::cout << "Samplerate " << (*sfinfo).samplerate << std::endl;
	std::cout << "Channels   " << (*sfinfo).channels << std::endl;
	std::cout << "Format     " << (*sfinfo).format << std::endl;
	std::cout << "Sections   " << (*sfinfo).sections << std::endl;
	std::cout << "Seekable   " << (*sfinfo).seekable << std::endl;
}

int sf_errno;
int AudioFile::setup(const std::string& path, size_t bufferSize, Mode mode, size_t channels /* = 0 */, unsigned int sampleRate /* = 0 */)
{
	cleanup();
	int sf_mode;
	switch(mode){
	case kWrite:
		sf_mode = SFM_WRITE;
		sfinfo.samplerate = sampleRate;
		sfinfo.format = SF_FORMAT_RAW | SF_FORMAT_PCM_16;
		sfinfo.channels = channels;
		break;
	case kRead:
		sfinfo.format = 0;
		sf_mode = SFM_READ;
		break;
	}
	//sndfile = sf_open(path.c_str(), sf_mode, &sfinfo);
	std::string server = "192.168.7.1";
	std::string port = "4000";
	if(sf_socket_setup(&mysock, server.c_str(), port.c_str()))
	{
		fprintf(stderr, "Cannot connect to %s:%s\n", server.c_str(), port.c_str());
		return 1;
	}
	SF_VIRTUAL_IO sf_virtual_socket = {
		.get_filelen = sf_socket_get_filelen,
		.seek = sf_socket_seek,
		.read = sf_socket_read,
		.write = sf_socket_write,
		.tell = sf_socket_tell,
	};
	sndfile = sf_open_virtual(&sf_virtual_socket, sf_mode, &sfinfo, &mysock);
	print_info(&sfinfo);
	printf("sndfile: %p %d\n", sndfile, sf_errno);
	if(!sndfile)
		return 1;
	rtIdx = 0;
	ramOnly = false;
	ioBuffer = 0;
	size_t numSamples = getLength() * getChannels();
	if(kRead == mode && bufferSize * kNumBufs >= numSamples)
	{
		ramOnly = true;
		// empty other buffers, we will only use the first one
		for(unsigned int n = 1; n < internalBuffers.size(); ++n)
			internalBuffers[n].clear();
		// the actual audio content
		internalBuffers[0].resize(numSamples);
	} else {
		for(auto& b : internalBuffers)
			b.resize(bufferSize * getChannels());
	}
	ioBufferOld = ioBuffer;
	if(kRead == mode)
	{
		// fill up the first buffer
		io(internalBuffers[ioBuffer]);
		// signal threadLoop() to start filling in the next buffer
		scheduleIo();
	}
	if(!ramOnly)
	{
		stop = false;
		diskIo = std::thread(&AudioFile::threadLoop, this);
	}
	return 0;
}

void AudioFile::cleanup()
{
	stop = true;
	if(diskIo.joinable())
		diskIo.join();
	sf_close(sndfile);
}

void AudioFile::scheduleIo()
{
	// schedule thread
	// TODO: detect underrun
	ioBuffer = !ioBuffer;
}

void AudioFile::threadLoop()
{
	while(!stop)
	{
		if(ioBuffer != ioBufferOld)
			io(internalBuffers[ioBuffer]);
		ioBufferOld = ioBuffer;
		usleep(100000);
	}
}

AudioFile::~AudioFile()
{
	cleanup();
}

int AudioFileReader::setup(const std::string& path, size_t bufferSize)
{
	loop = false;
	return AudioFile::setup(path, bufferSize, kRead);
}

int AudioFileReader::setLoop(bool doLoop)
{
	return setLoop(0, getLength() * !!doLoop);
}

int AudioFileReader::setLoop(size_t start, size_t end)
{
	if(start == end)
	{
		this->loop = false;
		return 0;
	}
	if(start > getLength() || end > getLength() || end < start)
		return 1;
	this->loop = true;
	loopStart = start;
	loopStop = end;
	return 0;
}

void AudioFileReader::io(std::vector<float>& buffer)
{
	size_t count = buffer.size();
	size_t dstPtr = 0;
	int readcount = 0;
	while(count)
	{
		size_t toRead = count;
		if(loop)
		{
			size_t stop = loopStop * getChannels();
			if(stop > idx)
			{
				size_t toEndOfLoop = stop - idx;
				if(toRead > toEndOfLoop)
					toRead = toEndOfLoop;
			} else {
				// force to return 0 and trigger a loop rewind
				toRead = 0;
			}
		}
		int readcount = sf_read_float(sndfile, buffer.data() + dstPtr, toRead);
		idx += readcount;
		dstPtr += readcount;
		count -= readcount;
		if(count)
		{
			// end of file or of loop section:
			if(loop)
			{
				// rewind and try again
				sf_seek(sndfile, loopStart, SEEK_SET);
				idx = loopStart;
			}
			else
			{
				// fill the rest with zeros
				memset(buffer.data() + dstPtr, 0, count * sizeof(buffer[0]));
				count = 0;
			}
		}
	}
}

void AudioFileReader::getSamples(std::vector<float>& outBuf)
{
	return getSamples(outBuf.data(), outBuf.size());
}

void AudioFileReader::getSamples(float* dst, size_t samplesCount)
{
	size_t n = 0;
	while(n < samplesCount)
	{
		auto& inBuf = getRtBuffer();
		size_t inBufEnd = ramOnly ?
			(loop ? loopStop * getChannels() : inBuf.size())
			: inBuf.size();
		bool done = false;
		for(; n < samplesCount && rtIdx < inBufEnd; ++n)
		{
			done = true;
			dst[n] = inBuf[rtIdx++];
		}
		if(rtIdx == inBufEnd)
		{
			if(ramOnly)
			{
				if(loop)
					rtIdx = loopStart;
				else {
					memset(dst + n, 0, (samplesCount - n) * sizeof(dst[0]));
					n = samplesCount;
					done = true;
				}
			} else {
				rtIdx = 0;
				scheduleIo(); // this should give us a new inBuf
			}
		}
		if(!done){
			break;
		}
	}
}

int AudioFileWriter::setup(const std::string& path, size_t bufferSize, size_t channels, unsigned int sampleRate)
{
	return AudioFile::setup(path, bufferSize, kWrite, channels, sampleRate);
}

void AudioFileWriter::setSamples(std::vector<float>& buffer)
{
	return setSamples(buffer.data(), buffer.size());
}

void AudioFileWriter::setSamples(float const * src, size_t samplesCount)
{
	size_t n = 0;
	while(n < samplesCount)
	{
		auto& outBuf = getRtBuffer();
		bool done = false;
		for(; n < samplesCount && rtIdx < outBuf.size(); ++n)
		{
			done = true;
			outBuf[rtIdx++] = src[n];
		}
		if(rtIdx == outBuf.size())
		{
			rtIdx = 0;
			scheduleIo(); // this should give us a new outBuf
		}
		if(!done){
			break;
		}
	}
}

void AudioFileWriter::io(std::vector<float>& buffer)
{
	size_t count = buffer.size();
	sf_count_t ret = sf_write_float(sndfile, buffer.data(), buffer.size());
	if(ret != sf_count_t(buffer.size()))
		fprintf(stderr, "Error while writing to file: %lld\n", ret);
}
