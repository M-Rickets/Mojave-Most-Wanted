// People in the car
#pragma once
#include "rt.h"

namespace rider
{
enum Bone
{
	PELVIS, SPINE, NECK, HEAD,
	L_THIGH, L_CALF, L_FOOT, L_TOE, R_THIGH, R_CALF, R_FOOT, R_TOE,
	L_UPPER, L_FORE, L_HAND, L_FINGER, R_UPPER, R_FORE, R_HAND, R_FINGER,
	kBones
};
struct Body
{
	u32 root;
	u32 bone[kBones];
	float orig[kBones][9];
	float headScale;
	float fix[9];
	float scale;
	u32 finger[2][5][3];
	float fingerOrig[2][5][3][9];
	bool fadeNode;
	float rootLocal[13];
};
struct Pose
{
	float eye[3];
	int side;
	float floorZ;
	float recline;
	bool wheel;
	float wheelC[3], wheelA[3], wheelR, wheelAngle;
	float shift;
	float lever[3];
	bool hideHead;
	float curl;
	float handsFollow;
	float lean;
	float headYaw, headPitch, headRoll;
};
struct Free
{
	float eye[3];
	float lean;
	float hand[2][3], handDir[2][3], handSide[2][3], curl[2];
	float foot[2][3], kneePole[2][3];
	bool hideHead;
	float tilt;
};
void poseFree(Body &b, const Free &p);
void unpose(const Body &b);
void parkourEye(int kind, float u, float eyeH, float gap, float wall, float h, float standY, float *y, float *z);
void parkourLook(const Free &F, int kind, float u, float *target, float *w);
void parkour(Free &F, int kind, float u, float t, const float *eye, float eyeH, float wall, float h, float standY, bool third, const float (*grip)[3] = 0, const float *anchor = 0);
float dropkickStandY();
float dropkickLie(float t, float u, float G);
u32 copy(u32 obj, bool prune = true);
void lastCopyMs(float out[5]);
void lastPruned(int out[4]);
bool lastCopyFaulted();
int lastLifted();
void scanSpecial(u32 root, int out[4]);
void hold(u32 obj);
void release(u32 obj);
void attach(u32 parent, u32 child);
void detach(u32 parent, u32 child);
u32 findByName(u32 root, const char *name);
bool bind(Body &b, u32 root);
void pose(Body &b, const Pose &p);
int skinCheck(u32 root, int *outside);
void bonePos(const Body &b, int bone, float out[3]);
}
