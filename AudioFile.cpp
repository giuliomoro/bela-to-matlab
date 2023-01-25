#include "AudioFile.h"
#include <unistd.h>
#include <string.h>
#include <MiscUtilities.h>

// socket code adapted from https://riptutorial.com/cplusplus/example/24000/hello-tcp-client
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <linux/sockios.h> // SIOCOUTQ
//#define USE_POLL
#ifdef USE_POLL
#include <poll.h>
#endif // USE_POLL

class TcpSocket {
private:
	int sock = -1;
	bool autoReconnect;
	bool isConnected = false;
	size_t sockBufferSize;
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
		isConnected = false;
		if(sock >= 0)
			close(sock);
		// socket() call creates a new socket and returns its descriptor
		sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (sock < 0) {
			fprintf(stderr, "Error while creating socket\n");
			return 1;
		}
		const unsigned int kReqBufferSize = 1 << 23;
		int ret = setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &kReqBufferSize, sizeof(kReqBufferSize));
		unsigned int sz;
		socklen_t actualLength = sizeof(sz);
		ret |= getsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sz, &actualLength);
		if(ret) {
			fprintf(stderr, "{s,g}etsockopt() failed\n");
			sockBufferSize = 1; // dummy
		} else {
			// this / 2 is found experimentally by looking at the buffer filling up:
			// It is known that Linux will allocate twice the memory requested via setsockopt()
			// but I would expect the value returned by
			// getsockopt() would be the real one; however we see
			// that the buffer fills up at sz/2
			sockBufferSize = sz / 2;
		}
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
		// increase max write socket buffer size
		IoUtils::writeTextFile("/proc/sys/net/core/wmem_max", std::to_string(1024 * 1024 * 64)); // 64MiB
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
#ifdef USE_POLL
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
#else
				// we use socket() because it tells us how long we slept for
				// and we can use that to detect anomalies
				fd_set set;
				FD_ZERO(&set);
				FD_SET(sock, &set);
				const unsigned long int kInitialTimeout = 100000;
				struct timeval timeout = {
					.tv_sec = 0,
					.tv_usec = kInitialTimeout,
				};
				int pollRet = select(sock + 1, NULL, &set, NULL, &timeout);
				float to = (kInitialTimeout - timeout.tv_usec) / 1000.f;
				if(to > 10)
					printf("to: %.0fms -- ", to);
#endif
				if(1 == pollRet)
				{
#ifdef USE_POLL
					if(pollfds.revents & POLLERR)
						fprintf(stderr, "poll() returned POLLERR\n");
#endif
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
				fprintf(stderr, "Could not write %zu bytes, buffer level is at %.2f%%\n", size, writeStat() * 100);
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
	float writeStat()
	{
		size_t val;
		int ioret = ioctl(sock, SIOCOUTQ, &val);
		if(!ioret)
			return val / float(sockBufferSize);
		else
			return -1;
	}
};

struct AudioFile::VirtualData {
	TcpSocket socket;
	unsigned int count;
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
	this->path = path;
	if(StringUtils::split(path, ':', true).size() == 2)
		mode = kWriteSocket;
	cleanup();
	int sf_mode;
	if(kWrite == mode || kWriteSocket == mode)
	{
		sf_mode = SFM_WRITE;
		sfinfo.samplerate = sampleRate;
		sfinfo.channels = channels;
	} else if(kRead == mode) {
		sfinfo.format = 0;
		sf_mode = SFM_READ;
	}
	if(kWrite == mode)
		sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
	else if(kWriteSocket == mode)
		sfinfo.format = SF_FORMAT_RAW | SF_FORMAT_PCM_16;
	if(kWrite == mode || kRead == mode)
	{
		sndfile = sf_open(path.c_str(), sf_mode, &sfinfo);
	} else if (kWriteSocket == mode) {
		std::vector<std::string> tokens = StringUtils::split(path, ':');
		std::string server = tokens[0];
		std::string port = tokens[1];
		virtualData = new VirtualData;
		virtualData->count = 0;
		if(virtualData->socket.setup(server, port))
		{
			fprintf(stderr, "Cannot connect to %s:%s, will try again later\n", server.c_str(), port.c_str());
		}
		printBufferDetails = true;
		SF_VIRTUAL_IO sf_virtual_socket = {
			.get_filelen = sf_socket_get_filelen,
			.seek = sf_socket_seek,
			.read = sf_socket_read,
			.write = sf_socket_write,
			.tell = sf_socket_tell,
		};
		sndfile = sf_open_virtual(&sf_virtual_socket, sf_mode, &sfinfo, virtualData);
		print_info(&sfinfo);
	}
	if(!sndfile)
	{
		fprintf(stderr, "Error while opening sndfile at %s\n", path.c_str());
		return 1;
	}
	rtIdx = 0;
	ramOnly = false;
	size_t numSamples = getLength() * getChannels();
	size_t numBufs = 10;
	rtBuffer = 0;
	ioBuffer = 0;
	if(kRead == mode && bufferSize * numBufs >= numSamples)
	{
		ramOnly = true;
		// empty other buffers, we will only use the first one
		internalBuffers.resize(1);
		// the actual audio content
		internalBuffers[0].resize(numSamples);
	} else {
		internalBuffers.resize(numBufs);
		for(auto& b : internalBuffers)
			b.resize(bufferSize * getChannels());
	}
	if(kRead == mode)
	{
		// fill up the first buffer
		io(internalBuffers[0]);
		ioBuffer = 1;
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
	sndfile = NULL;
	delete virtualData;
	virtualData = nullptr;
}

extern "C" {
int rt_fprintf(FILE *stream, const char *format, ...);
};

void AudioFile::scheduleIo()
{
	// here you could have a cond var or mutex that the io thread is blocking on.
	// in our case we have a simple (and technically unsafe) shared variable
	increment(rtBuffer);
	if(rtBuffer == ioBuffer)
	{
		// if you don't have rt_fprintf, turn this into fprintf
		if(virtualData)
			rt_fprintf(stderr, "_______%.2f%%\n", virtualData->socket.writeStat() * 100);
		rt_fprintf(stderr, "AudioFile: underrun detected on %s\n", path.c_str());
	}
}

static const long long unsigned int kNsInSec = 1000000000;
static const long long unsigned int kNsInMs = 1000000;
static long long unsigned int timespec_sub(const struct timespec *a, const struct timespec *b)
{
	long long unsigned int diff;
	diff = (a->tv_sec - b->tv_sec) * kNsInSec;
	diff += a->tv_nsec - b->tv_nsec;
	return diff;
}

static long long unsigned int timespec_to_llu(const struct timespec *a)
{
	return a->tv_sec * kNsInSec + a->tv_nsec;
}

#include <time.h>

template <typename T>
void AudioFile::increment(T& bufferIdx)
{
	bufferIdx = (bufferIdx + 1) % internalBuffers.size();
}

void AudioFile::threadLoop()
{
	bool inited = false;
	long long unsigned int totalBusy = 0;
	struct timespec start;
	printBufferDetails = 1;
	while(!stop)
	{
		if(ioBuffer != rtBuffer)
		{
			if(printBufferDetails && virtualData)
				printf("buf: %5.2f%% -- ", virtualData->socket.writeStat() * 100);
			struct timespec begin;
			struct timespec end;
			if(printBufferDetails)
			{
				if(clock_gettime(CLOCK_MONOTONIC, &begin))
					fprintf(stderr, "Error in clock_gettime(): %d %s\n", errno, strerror(errno));
				if(!inited)
				{
					start = begin;
					inited = true;
				}
			}
			io(internalBuffers[ioBuffer]);
			increment(ioBuffer);
			if(printBufferDetails)
			{
				if(clock_gettime(CLOCK_MONOTONIC, &end))
					fprintf(stderr, "Error in clock_gettime(): %d %s\n", errno, strerror(errno));
				if(virtualData)
					printf("%5.2f%% ", virtualData->socket.writeStat() * 100);
				long long unsigned int busy = timespec_sub(&end, &begin);
				double busyMs = busy / double(kNsInMs);
				long long unsigned int totalRunning = timespec_sub(&end, &start);
				printf("timeInIo: %6.1fms (%5.2f%% av)\n", busyMs, totalBusy/double(totalRunning) * 100.f);
				totalBusy += busy;
				//printf("start: %llu, begin:%llu, end: %llu, totalBusy: %llu, totalRunning: %llu\n", timespec_to_llu(&start), timespec_to_llu(&begin), timespec_to_llu(&end), totalBusy, totalRunning);
			}
		}
		else
			usleep(50000);
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
		auto& inBuf = internalBuffers[rtBuffer];
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
				scheduleIo(); // this will give us a new rtBuffer
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
		auto& outBuf = internalBuffers[rtBuffer];
		bool done = false;
		for(; n < samplesCount && rtIdx < outBuf.size(); ++n)
		{
			done = true;
			outBuf[rtIdx++] = src[n];
		}
		if(rtIdx == outBuf.size())
		{
			rtIdx = 0;
			scheduleIo(); // this will give us a new rtBuffer
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
