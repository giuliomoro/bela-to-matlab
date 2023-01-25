#include <Bela.h>
#include "AudioFile.h"
#include <vector>
#include <libraries/Oscillator/Oscillator.h>
std::vector<Oscillator> oscs;

std::string gFilename = "recording.wav";
std::string gServer = "192.168.1.1:4000";
AudioFileWriter* fileOut;
AudioFileWriter* serverOut;
std::vector<float> fileBuffer;

unsigned int chs = 8;
// size of internal buffer for writing to network and file.
// It affects latency.
// Increase if you get `AudioFile: underrun detected`
unsigned int bufferFrames = 65536 * 2;

bool setup(BelaContext *context, void *userData)
{
	fileOut = new AudioFileWriter;
	if(fileOut->setup(gFilename, bufferFrames, chs, context->audioSampleRate))
		return false;
	serverOut = new AudioFileWriter;
	if(serverOut->setup(gServer, bufferFrames, chs, context->audioSampleRate))
		return false;
	printf("Number of chs to disk %d\n", chs);
	fileBuffer.resize(context->audioFrames * chs);
	for(size_t c = 0; c < chs; ++c)
	{
		oscs.emplace_back(context->audioSampleRate);
		oscs[c].setFrequency(1 * (1 + c));
	}
	return true;
}

void render(BelaContext *context, void *userData)
{
	for(unsigned int n = 0; n < context->audioFrames; n++)
	{
		for(unsigned int c = 0; c < chs; ++c)
		{
			//float out = oscs[c].process();
			//fileBuffer[n * chs + c] = audioRead(context, n, c % context->audioInChannels);
			//fileBuffer[n * chs + c] = out;
			fileBuffer[n * chs + c] = (((context->audioFramesElapsed + n + c * 100) % 65536) - 32768) / 32768.f;
		}
	}
	fileOut->setSamples(fileBuffer.data(), fileBuffer.size());
	serverOut->setSamples(fileBuffer.data(), fileBuffer.size());
}

void cleanup(BelaContext *context, void *userData)
{
	delete fileOut;
	delete serverOut;
}
