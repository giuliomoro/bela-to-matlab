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
#include <poll.h>
class TcpSocket {
private:
	int sock = -1;
	bool autoReconnect;
	bool isConnected = false;
	addrinfo hints;
	addrinfo* p = nullptr;
	void cleanup()
	{
		freeaddrinfo(p);
		p = nullptr;
		if(sock >= 0)
			close(sock);
		sock = -1;
	}

	int doConnect()
	{
		printf("doconnect\n");
		isConnected = false;
		close(sock);
		// socket() call creates a new socket and returns its descriptor
		sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sock < 0) {
			fprintf(stderr, "Error while creating socket\n");
			return 1;
		}
		const unsigned int kBufferSize = 1 << 22;
		int ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &kBufferSize, sizeof(kBufferSize));
		ret |= setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &kBufferSize, sizeof(kBufferSize));
		unsigned int sz;
		socklen_t actualLength = sizeof(sz);
		ret |= getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sz, &actualLength);
		if(ret)
			fprintf(stderr, "setsockopt() failed\n");
		//printf("retrived si %d %d\n", ret, sz);
		// connect() call tries to establish a TCP connection to the specified server
		if(connect(sock, p->ai_addr, p->ai_addrlen))
		{
			fprintf(stderr, "Error while connecting socket: %d %s\n", errno, strerror(errno));
			return 1;
		}
		isConnected = true;
		return 0;
	}
public:
	~TcpSocket()
	{
		cleanup();
	}

	int setup(const std::string& ipAddress, const std::string& portNum, bool autoReconnect = true)
	{
		cleanup();
		this->autoReconnect = autoReconnect;
		memset(&hints, 0, sizeof(hints));
		hints.ai_family   = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags	= AI_PASSIVE;

		int gAddRes = getaddrinfo(ipAddress.c_str(), portNum.c_str(), &hints, &p);
		if (gAddRes != 0) {
			fprintf(stderr, "%s\n", gai_strerror(gAddRes));
			return 1;
		}

		if (p == NULL) {
			fprintf(stderr, "No addresses found\n");
			return 1;
		}
		return doConnect();
	}
	int write(const void* data, size_t size)
	{
		unsigned int n = 2;
		int ret = -1;
		while(n--)
		{
			if(isConnected)
			{
				struct pollfd pollfds = {
					.fd = sock,
					.events = POLLOUT,
					// according to the manual, one should poll()
					// for POLLIN and then read with recv() and
					// obtain a 0 and that would mean that the peer
					// has shut down the connection. But this
					// doesn't seem to work, so we detect a remote's
					// disconnection when an error happens while writing
					// (which may happen after the socket's buffer has
					// been filled)
				};
				nfds_t nfds = 1;
				int pollRet = poll(&pollfds, nfds, 100);
				if(1 == pollRet)
				{
					if(pollfds.revents & POLLERR)
						fprintf(stderr, "poll() returned POLLERR\n");
					ret = send(sock, data, size, MSG_NOSIGNAL); // MSG_NOSIGNAL: avoid SIGPIPE if remote has closed connection
					if(ret > 0) // all good!
						break;
					else {
						isConnected = false;
						fprintf(stderr, "send() returned %d %s\n", errno, strerror(errno));
					}
				} else
					// probably ran out of space in the socket's buffer
					isConnected = false;
			}
			if(!isConnected && autoReconnect && (1 == n))
			{
				// something failed (just now or earlier). Give
				// it a chance: try connect and try sending again in
				// the next iteration of the loop
				doConnect();
			}
		}
		return ret;
	}
	int read(void* data, size_t size)
	{
		// TODO: handle reconnection
		return recv(sock, data, size, 0);
	}
};

struct socket_data_t {
	TcpSocket socket;
	unsigned int count;
};

static inline socket_data_t* to_socket_data(void* user_data)
{
	return static_cast<socket_data_t*>(user_data);
}

static sf_count_t sf_socket_get_filelen(void *user_data)
{
	socket_data_t* data = to_socket_data(user_data);
	return data->count;
}

static sf_count_t sf_socket_seek(sf_count_t offset, int whence, void *user_data)
{
	return ESPIPE;
}

static sf_count_t sf_socket_read(void *ptr, sf_count_t count, void *user_data)
{
	socket_data_t* data = to_socket_data(user_data);
	int ret = data->socket.read(ptr, count);
	if(ret >= 0)
		data->count += ret;
	return ret;
}

static sf_count_t sf_socket_write(const void *ptr, sf_count_t count, void *user_data)
{
	socket_data_t* data = to_socket_data(user_data);
	int ret = data->socket.write(ptr, count);
	if(ret >= 0)
		data->count += ret;
	else
		fprintf(stderr, "sf_socket_write: %d\n", ret);
	return ret;
}

static sf_count_t sf_socket_tell(void *user_data)
{
	socket_data_t* data = to_socket_data(user_data);
	return data->count;
}

static socket_data_t mydata = {
	.count = 0,
};

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
	if(mydata.socket.setup(server, port))
	{
		fprintf(stderr, "Cannot connect to %s:%s, will try again later\n", server.c_str(), port.c_str());
	}
	SF_VIRTUAL_IO sf_virtual_socket = {
		.get_filelen = sf_socket_get_filelen,
		.seek = sf_socket_seek,
		.read = sf_socket_read,
		.write = sf_socket_write,
		.tell = sf_socket_tell,
	};
	sndfile = sf_open_virtual(&sf_virtual_socket, sf_mode, &sfinfo, &mydata);
	print_info(&sfinfo);
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
		fflush(stdout);
		if(ioBuffer != ioBufferOld)
		{
			io(internalBuffers[ioBuffer]);
			ioBufferOld = ioBuffer;
		}
		else
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
	sf_count_t ret = sf_write_float(sndfile, buffer.data(), buffer.size());
	if(ret != sf_count_t(buffer.size()))
		fprintf(stderr, "Error while writing to file: %lld\n", ret);
}
