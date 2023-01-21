#include <Bela.h>
#include "AudioFile.h"
#include <vector>
#include <libraries/Oscillator/Oscillator.h>
std::vector<Oscillator> oscs;


std::string gFilename = "recording.wav";
AudioFileWriter* fileOut;
std::vector<float> fileBuffer;

unsigned int chs = 16;
// how many samples to write to disk at once
unsigned int bufferFrames = 8192;

bool setup(BelaContext *context, void *userData)
{
	fileOut = new AudioFileWriter;
	if(fileOut->setup(gFilename, bufferFrames, chs, context->audioSampleRate))
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
			float out = oscs[c].process();
			//fileBuffer[n * chs + c] = audioRead(context, n, c % context->audioInChannels);
			fileBuffer[n * chs + c] = out;
		}
	}
	fileOut->setSamples(fileBuffer.data(), fileBuffer.size());
}

void cleanup(BelaContext *context, void *userData)
{
	delete fileOut;
}
