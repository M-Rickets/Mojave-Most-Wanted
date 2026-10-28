// Mojave Most Wanted audio
#pragma once
#include "rt.h"

namespace audio
{
bool start(const char *pluginDir);
void stop();
bool loadEngine(const char *carId);
void unloadEngine();
void setEngine(bool on, float rpm, float throttle, float volume);
void setMaster(float effects, float radio);
void setRadioPan(float pan);
void setSkid(float amount, float pitch, float volume);
void setWind(float level, float muffled);
void setRain(float level, float muffled);
void setSkidLayers(float body, float mid, float high);
bool haveSkids();
void playShot(int which, float volume, float pitch);
void playClick(int kind, float volume, float pitch);
void setLimiter(bool on, float hz, float drop, float cut);
void setExternalBass(bool on, float push);
void setAnalyze(bool on);
void bands(float *out);
float engineMinRPM();
float engineMaxRPM();
void radioPlay(const char *mp3Path);
void radioStop();
void radioPause(bool paused);
bool radioFinished();
void radioVolumeState(u32 *level, u32 *mciResult);
bool radioDecoded();
void setSubwoofer(int level, const float *tuning);
void setRadioOutside(float outside);
float bassEnv();
}
