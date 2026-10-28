#include "rider.h"

// Scene objects (JIP LN internal/netimmerse.h)
namespace rider
{
static inline u32 P(u32 a) { return a ? *(u32 *)a : 0; }
static bool isNode(u32 o)
{
	u32 vt = o ? P(o) : 0;
	return vt && P(vt + 0x0C) == 0x6815C0;
}
static void each(u32 node, void (*f)(u32, void *), void *ctx, int depth = 0)
{
	if (!node || depth > 64) return;
	f(node, ctx);
	if (!isNode(node)) return;
	u32 data = P(node + 0xA0);
	u16 n = *(u16 *)(node + 0xA6);
	for (u16 i = 0; data && i < n && i < 512; i++) each(P(data + 4 * i), f, ctx, depth + 1);
}
static void stripOne(u32 o, void *)
{
	*(u32 *)(o + 0x0C) = 0;
	*(u32 *)(o + 0x1C) = 0;
	*(u32 *)(o + 0x30) &= ~0x00100000u;
}
static const char *rttiName(u32 o)
{
	u32 vt = P(o);
	if (!vt || !P(vt + 8)) return 0;
	u32 r = (u32)(unsigned long)((void *(__thiscall *)(void *))P(vt + 8))((void *)o);
	return r ? (const char *)P(r) : 0;
}
static bool contains(const char *s, const char *w)
{
	if (!s) return false;
	for (; *s; s++)
	{
		const char *a = s, *b = w;
		while (*a && *b && (*a | 32) == (*b | 32)) { a++; b++; }
		if (!*b) return true;
	}
	return false;
}
struct Prune { u32 parent[64], child[64]; int n, lights, particles, flashes, guns; };
static void pruneScan(u32 node, Prune &pr, int depth)
{
	if (!node || depth > 64 || !isNode(node)) return;
	u32 data = P(node + 0xA0);
	u16 n = *(u16 *)(node + 0xA6);
	const char *pn = (const char *)P(node + 8);
	bool gunHolder = pn && !rt::strcmp(pn, "Weapon");
	for (u16 i = 0; data && i < n && i < 512; i++)
	{
		u32 c = P(data + 4 * i);
		if (!c) continue;
		const char *rt_ = rttiName(c), *nm = (const char *)P(c + 8);
		int why = gunHolder ? 4 : contains(rt_, "Light") ? 1 : (contains(rt_, "Particle") || contains(rt_, "PSys")) ? 2 : contains(nm, "MuzzleFlash") ? 3 : 0;
		if (why && pr.n < 64)
		{
			pr.parent[pr.n] = node; pr.child[pr.n] = c; pr.n++;
			if (why == 1) pr.lights++; else if (why == 2) pr.particles++; else if (why == 3) pr.flashes++; else pr.guns++;
			continue;
		}
		pruneScan(c, pr, depth + 1);
	}
}
static int g_lastPrune[4];
static int g_preLifted;
int lastLifted() { return g_preLifted; }
void scanSpecial(u32 root, int out[4])
{
	static Prune pr;
	memset(&pr, 0, sizeof pr);
	if (root) pruneScan(root, pr, 0);
	out[0] = pr.lights; out[1] = pr.particles; out[2] = pr.flashes; out[3] = pr.guns;
}
void lastPruned(int out[4]) { for (int i = 0; i < 4; i++) out[i] = g_lastPrune[i]; }
extern "C" { extern volatile u32 g_gFault, g_gThread; u32 __cdecl guardedCall2(u32 fn, u32 a, u32 b); }
static u32 __cdecl cloneThunk(u32 obj, u32 ci) { void **vt = *(void ***)obj; return (u32)(unsigned long)((void *(__thiscall *)(void *, void *))vt[0x48 / 4])((void *)obj, (void *)ci); }
static u32 __cdecl fixThunk(u32 obj, u32 ci) { void **vt = *(void ***)obj; ((void(__thiscall *)(void *, void *))vt[0x68 / 4])((void *)obj, (void *)ci); return 1; }
static bool g_copyFaulted;
bool lastCopyFaulted() { return g_copyFaulted; }
static float g_copyMs[5];
void lastCopyMs(float out[5]) { for (int i = 0; i < 5; i++) out[i] = g_copyMs[i]; }
u32 copy(u32 obj, bool prune)
{
	if (!obj) return 0;
	double c0 = rt::now();
	for (int i = 0; i < 5; i++) g_copyMs[i] = 0;
	static Prune pre;
	memset(&pre, 0, sizeof pre);
	if (prune) pruneScan(obj, pre, 0);
	int slot[64];
	for (int i = 0; i < pre.n; i++)
	{
		slot[i] = -1;
		u32 data = P(pre.parent[i] + 0xA0);
		u16 cnt = *(u16 *)(pre.parent[i] + 0xA6);
		for (u16 k = 0; data && k < cnt && k < 512; k++) if (P(data + 4 * k) == pre.child[i]) slot[i] = k;
		hold(pre.child[i]);
		if (slot[i] >= 0) detach(pre.parent[i], pre.child[i]);
	}
	struct { u32 map00, map04, unk08; u8 b0C, pad[3]; float scale[3]; u32 spare[9]; } ci;
	memset(&ci, 0, sizeof ci);
	((void(__thiscall *)(void *, u32))0x4AD0C0)(&ci, 0x97);
	ci.scale[0] = ci.scale[1] = ci.scale[2] = 1;
	g_gThread = GetCurrentThreadId();
	double c1 = rt::now();
	u32 c = guardedCall2((u32)&cloneThunk, obj, (u32)&ci);
	bool faulted = g_gFault != 0;
	double c2 = rt::now();
	if (!faulted && c)
	{
		guardedCall2((u32)&fixThunk, obj, (u32)&ci);
		if (g_gFault) faulted = true;
	}
	double c3 = rt::now();
	g_copyMs[0] = (float)((c1 - c0) * 1000); g_copyMs[1] = (float)((c2 - c1) * 1000); g_copyMs[2] = (float)((c3 - c2) * 1000);
	g_copyFaulted = faulted;
	if (faulted) c = 0;
	for (int i = pre.n - 1; i >= 0; i--)
	{
		if (slot[i] >= 0)
		{
			void **pv = *(void ***)pre.parent[i];
			((void(__thiscall *)(void *, u32, void *))pv[0xE0 / 4])((void *)pre.parent[i], (u32)slot[i], (void *)pre.child[i]);
		}
		release(pre.child[i]);
	}
	g_preLifted = pre.n;
	if (!c) return 0;
	double c4 = rt::now();
	each(c, stripOne, 0);
	double c5 = rt::now();
	g_copyMs[3] = (float)((c5 - c4) * 1000);
	*(u32 *)(c + 0x30) &= ~1u;
	static Prune pr;
	memset(&pr, 0, sizeof pr);
	if (prune) pruneScan(c, pr, 0);
	for (int i = 0; i < pr.n; i++) detach(pr.parent[i], pr.child[i]);
	g_lastPrune[0] = pr.lights; g_lastPrune[1] = pr.particles; g_lastPrune[2] = pr.flashes; g_lastPrune[3] = pr.guns;
	g_copyMs[4] = (float)((rt::now() - c5) * 1000);
	return c;
}
void hold(u32 obj) { if (obj) __sync_fetch_and_add((u32 *)(obj + 4), 1); }
void release(u32 obj)
{
	if (obj && __sync_sub_and_fetch((u32 *)(obj + 4), 1) == 0)
	{
		void **vt = *(void ***)obj;
		((void(__thiscall *)(void *))vt[1])((void *)obj);
	}
}
void attach(u32 parent, u32 child)
{
	if (!parent || !child) return;
	void **vt = *(void ***)parent;
	((void(__thiscall *)(void *, void *, int))vt[0xDC / 4])((void *)parent, (void *)child, 1);
}
void detach(u32 parent, u32 child)
{
	if (!parent || !child) return;
	void **vt = *(void ***)parent;
	((void(__thiscall *)(void *, void *))vt[0xE8 / 4])((void *)parent, (void *)child);
}
struct Find { const char *name; u32 hit; };
static void findOne(u32 o, void *ctx)
{
	Find &f = *(Find *)ctx;
	const char *nm = f.hit ? 0 : (const char *)P(o + 8);
	if (nm && !rt::strcmp(nm, f.name)) f.hit = o;
}
u32 findByName(u32 root, const char *name)
{
	Find f = {name, 0};
	each(root, findOne, &f);
	return f.hit;
}

struct V { float x, y, z; };
static inline V v3(float x, float y, float z) { V r = {x, y, z}; return r; }
static inline V add(V a, V b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline V sub(V a, V b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline V mul(V a, float k) { return v3(a.x * k, a.y * k, a.z * k); }
static inline float dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline V cross(V a, V b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static inline float len(V a) { return rt::sqrt(dot(a, a)); }
static inline V norm(V a) { float l = len(a); return l > 1e-6f ? mul(a, 1 / l) : v3(0, 0, 1); }
static inline V fromf(const float *f) { return v3(f[0], f[1], f[2]); }
static void mm(const float *a, const float *b, float *o)
{
	float t[9];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) t[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
	memcpy(o, t, sizeof t);
}
static V mv(const float *m, V v) { return v3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z); }
static void tr(const float *m, float *o) { float t[9] = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]}; memcpy(o, t, sizeof t); }
static void arc(V a, V b, float *r)
{
	float c = dot(a, b);
	if (c < -0.9999f)
	{
		V w = norm(rt::absf(a.x) < 0.9f ? cross(a, v3(1, 0, 0)) : cross(a, v3(0, 1, 0)));
		float m[9] = {2 * w.x * w.x - 1, 2 * w.x * w.y, 2 * w.x * w.z, 2 * w.y * w.x, 2 * w.y * w.y - 1, 2 * w.y * w.z, 2 * w.z * w.x, 2 * w.z * w.y, 2 * w.z * w.z - 1};
		memcpy(r, m, sizeof m);
		return;
	}
	V v = cross(a, b);
	float k = 1 / (1 + c), vv = dot(v, v);
	float m[9] = {1 + k * (v.x * v.x - vv), -v.z + k * v.x * v.y, v.y + k * v.x * v.z,
		v.z + k * v.y * v.x, 1 + k * (v.y * v.y - vv), -v.x + k * v.y * v.z,
		-v.y + k * v.z * v.x, v.x + k * v.z * v.y, 1 + k * (v.z * v.z - vv)};
	memcpy(r, m, sizeof m);
}

struct Xf { float r[9]; V t; float s; };
static void loc(u32 n, Xf &x)
{
	const float *f = (const float *)(n + 0x34);
	memcpy(x.r, f, 36);
	x.t = v3(f[9], f[10], f[11]);
	x.s = f[12];
}
static bool xf(const Body &b, u32 n, Xf &out, bool withRoot = true)
{
	u32 chain[64];
	int k = 0;
	for (u32 c = n; c && c != b.root; c = P(c + 0x18))
	{
		if (k >= 64) return false;
		chain[k++] = c;
	}
	Xf w;
	if (withRoot) loc(b.root, w);
	else { float id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; memcpy(w.r, id, 36); w.t = v3(0, 0, 0); w.s = 1; }
	if (n == b.root) { out = w; return true; }
	for (int i = k - 1; i >= 0; i--)
	{
		Xf l;
		loc(chain[i], l);
		Xf o;
		mm(w.r, l.r, o.r);
		o.t = add(w.t, mul(mv(w.r, l.t), w.s));
		o.s = w.s * l.s;
		w = o;
	}
	out = w;
	return true;
}
static V pos(const Body &b, int bone)
{
	Xf x;
	return b.bone[bone] && xf(b, b.bone[bone], x) ? x.t : v3(0, 0, 0);
}
struct Skin { u32 root; int n, outside; };
static void skinOne(u32 o, void *ctx)
{
	Skin &k = *(Skin *)ctx;
	if (isNode(o)) return;
	u32 si = P(o + 0xBC), bones = si ? P(si + 0x14) : 0, b0 = bones ? P(bones) : 0;
	if (!b0) return;
	k.n++;
	u32 c = b0;
	for (int i = 0; c && c != k.root && i < 64; i++) c = P(c + 0x18);
	if (c != k.root) k.outside++;
}
int skinCheck(u32 root, int *outside)
{
	Skin k = {root, 0, 0};
	each(root, skinOne, &k);
	*outside = k.outside;
	return k.n;
}
void bonePos(const Body &b, int bone, float out[3]) { V p = pos(b, bone); out[0] = p.x; out[1] = p.y; out[2] = p.z; }
static void turn(const Body &b, int bone, const float *r)
{
	u32 n = b.bone[bone];
	if (!n) return;
	Xf w, p;
	if (!xf(b, n, w) || !xf(b, P(n + 0x18), p)) return;
	float nw[9], pt[9], nl[9];
	mm(r, w.r, nw);
	tr(p.r, pt);
	mm(pt, nw, nl);
	memcpy((float *)(n + 0x34), nl, 36);
}
static void aim(const Body &b, int bone, V from, V at, V to)
{
	float r[9];
	arc(norm(sub(at, from)), norm(to), r);
	turn(b, bone, r);
}

static void turnNode(const Body &b, u32 n, const float *r)
{
	Xf w, p;
	if (!n || !xf(b, n, w) || !xf(b, P(n + 0x18), p)) return;
	float nw[9], pt[9], nl[9];
	mm(r, w.r, nw);
	tr(p.r, pt);
	mm(pt, nw, nl);
	memcpy((float *)(n + 0x34), nl, 36);
}
static V nodePos(const Body &b, u32 n)
{
	Xf x;
	return n && xf(b, n, x) ? x.t : v3(0, 0, 0);
}
static void rotAxis(V k, float a, float *m)
{
	float c = rt::cos(a), sn = rt::sin(a), t = 1 - c;
	float r[9] = {c + k.x * k.x * t, k.x * k.y * t - k.z * sn, k.x * k.z * t + k.y * sn,
		k.y * k.x * t + k.z * sn, c + k.y * k.y * t, k.y * k.z * t - k.x * sn,
		k.z * k.x * t - k.y * sn, k.z * k.y * t + k.x * sn, c + k.z * k.z * t};
	memcpy(m, r, sizeof r);
}
static void frameTo(V a, V b, V c, V d, float *r)
{
	V n1 = cross(a, b), n2 = cross(c, d);
	float m1[9] = {a.x, b.x, n1.x, a.y, b.y, n1.y, a.z, b.z, n1.z}, m2[9] = {c.x, d.x, n2.x, c.y, d.y, n2.y, c.z, d.z, n2.z}, t1[9];
	tr(m1, t1);
	mm(m2, t1, r);
}
static float twistAbout(const float *r, V f)
{
	float tr0 = r[0] + r[4] + r[8], w = rt::sqrt(rt::max(0, 1 + tr0)) * 0.5f;
	if (w < 1e-3f) return 0;
	V q = v3((r[7] - r[5]) / (4 * w), (r[2] - r[6]) / (4 * w), (r[3] - r[1]) / (4 * w));
	return 2 * rt::atan2(dot(q, f), w);
}

static const char *kNames[kBones] = {
	"Bip01 Pelvis", "Bip01 Spine", "Bip01 Neck", "Bip01 Head",
	"Bip01 L Thigh", "Bip01 L Calf", "Bip01 L Foot", "Bip01 L Toe0", "Bip01 R Thigh", "Bip01 R Calf", "Bip01 R Foot", "Bip01 R Toe0",
	"Bip01 L UpperArm", "Bip01 L Forearm", "Bip01 L Hand", "Bip01 L Finger2", "Bip01 R UpperArm", "Bip01 R Forearm", "Bip01 R Hand", "Bip01 R Finger2"};
void unpose(const Body &b)
{
	if (!b.root) return;
	for (int i = 0; i < kBones; i++) if (b.bone[i]) memcpy((float *)(b.bone[i] + 0x34), b.orig[i], 36);
	for (int h = 0; h < 2; h++)
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++) if (b.finger[h][f][j]) memcpy((float *)(b.finger[h][f][j] + 0x34), b.fingerOrig[h][f][j], 36);
	if (b.bone[HEAD]) *(float *)(b.bone[HEAD] + 0x64) = b.headScale;
	memcpy((float *)(b.root + 0x34), b.rootLocal, 52);
}
bool bind(Body &b, u32 root)
{
	memset(&b, 0, sizeof b);
	if (!root) return false;
	b.root = root;
	for (int i = 0; i < kBones; i++) b.bone[i] = findByName(root, kNames[i]);
	if (!b.bone[L_FINGER]) b.bone[L_FINGER] = findByName(root, "Bip01 L Finger1");
	if (!b.bone[R_FINGER]) b.bone[R_FINGER] = findByName(root, "Bip01 R Finger1");
	for (int i = 0; i < kBones; i++)
		if (!b.bone[i] && i != L_TOE && i != R_TOE && i != L_FINGER && i != R_FINGER) { b.root = 0; return false; }
	for (int i = 0; i < kBones; i++)
		if (b.bone[i]) memcpy(b.orig[i], (float *)(b.bone[i] + 0x34), 36);
	for (int h = 0; h < 2; h++)
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
			{
				char nm[32];
				rt::fmt(nm, sizeof nm, j ? "Bip01 %c Finger%d%d" : "Bip01 %c Finger%d", h ? 'R' : 'L', f, j);
				u32 n = findByName(root, nm);
				b.finger[h][f][j] = n;
				if (n) memcpy(b.fingerOrig[h][f][j], (float *)(n + 0x34), 36);
			}
	{
		u32 vt = P(root), fn = vt ? P(vt + 0x08) : 0, rtti = fn ? ((u32(__thiscall *)(void *))fn)((void *)root) : 0;
		const char *nm = rtti ? (const char *)P(rtti) : 0;
		b.fadeNode = nm && !rt::strcmp(nm, "BSFadeNode");
	}
	b.headScale = *(float *)(b.bone[HEAD] + 0x64);
	memcpy(b.rootLocal, (float *)(root + 0x34), 52);
	b.scale = *(float *)(root + 0x64);
	if (!(b.scale > 0.2f && b.scale < 5)) b.scale = 1;
	Xf l, r, pe, he;
	xf(b, b.bone[L_THIGH], l, false); xf(b, b.bone[R_THIGH], r, false);
	xf(b, b.bone[PELVIS], pe, false); xf(b, b.bone[HEAD], he, false);
	V x = sub(r.t, l.t), up = sub(he.t, pe.t);
	if (len(x) < 1e-3f || len(up) < 1e-3f) { b.root = 0; return false; }
	x = norm(x);
	V z = norm(sub(up, mul(x, dot(up, x)))), y = cross(z, x);
	float fix[9] = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
	memcpy(b.fix, fix, sizeof fix);
	return true;
}

static V ik(V j, V t, float l1, float l2, V pole)
{
	V d = sub(t, j);
	float dist = len(d);
	V u = dist > 1e-3f ? mul(d, 1 / dist) : v3(0, 1, 0);
	dist = rt::clamp(dist, rt::absf(l1 - l2) + 0.01f, l1 + l2 - 0.01f);
	float c = rt::clamp((l1 * l1 + dist * dist - l2 * l2) / (2 * l1 * dist), -1, 1), s = rt::sqrt(rt::max(0, 1 - c * c));
	V v = sub(pole, mul(u, dot(pole, u)));
	if (len(v) < 1e-4f) v = rt::absf(u.z) < 0.9f ? cross(u, v3(0, 0, 1)) : cross(u, v3(1, 0, 0));
	v = norm(v);
	return add(j, mul(add(mul(u, c), mul(v, s)), l1));
}
static void leg(const Body &b, int thigh, int calf, int foot, int toe, const Pose &p, V *knee, V *hip)
{
	V j = pos(b, thigh), k0 = pos(b, calf), f0 = pos(b, foot);
	float l1 = len(sub(k0, j)), l2 = len(sub(f0, k0)), total = l1 + l2;
	float dz = rt::clamp(j.z - (p.floorZ + 7), 0, 0.5f * total), reach = 0.86f * total;
	float fy = rt::sqrt(rt::max(reach * reach - dz * dz, 1));
	V t = v3(j.x, j.y + fy, j.z - dz);
	float out = thigh == L_THIGH ? -1.0f : 1.0f;
	V k = ik(j, t, l1, l2, v3(out * 0.25f, 0.35f, 1));
	aim(b, thigh, j, k0, sub(k, j));
	V k1 = pos(b, calf), f1 = pos(b, foot);
	aim(b, calf, k1, f1, sub(t, k1));
	if (b.bone[toe])
	{
		V f = pos(b, foot), tt = pos(b, toe);
		aim(b, foot, f, tt, v3(0, 0.9f, 0.42f));
	}
	*knee = k;
	*hip = j;
}
static void arm(const Body &b, int h, V wrist, V dir, V side, float curl)
{
	int upper = h ? R_UPPER : L_UPPER, fore = h ? R_FORE : L_FORE, hand = h ? R_HAND : L_HAND, finger = h ? R_FINGER : L_FINGER;
	V s = pos(b, upper), e0 = pos(b, fore), w0 = pos(b, hand);
	float l1 = len(sub(e0, s)), l2 = len(sub(w0, e0));
	float out = h ? 1.0f : -1.0f;
	V e = ik(s, wrist, l1, l2, v3(out * 0.35f, -0.2f, -1));
	aim(b, upper, s, e0, sub(e, s));
	V e1 = pos(b, fore), w1 = pos(b, hand);
	aim(b, fore, e1, w1, sub(wrist, e1));
	if (!b.bone[finger]) return;
	V lt = norm(dir), st = norm(sub(side, mul(lt, dot(side, lt))));
	u32 ix = b.finger[h][1][0], lf = b.finger[h][4][0];
	if (!ix || !lf)
	{
		V hp = pos(b, hand), fp = pos(b, finger);
		aim(b, hand, hp, fp, lt);
		return;
	}
	for (int pass = 0; pass < 2; pass++)
	{
		V hp = pos(b, hand), l = norm(sub(pos(b, finger), hp)), lat = sub(nodePos(b, ix), nodePos(b, lf));
		V t = norm(sub(lat, mul(l, dot(lat, l))));
		float r[9];
		frameTo(l, t, lt, st, r);
		if (pass == 0)
		{
			V f = norm(sub(hp, pos(b, fore)));
			float tw = twistAbout(r, f) * 0.6f, rf[9];
			rotAxis(f, tw, rf);
			turn(b, fore, rf);
		}
		else turn(b, hand, r);
	}
	if (curl <= 0) return;
	V palm = h ? cross(st, lt) : cross(lt, st);
	V k = norm(cross(lt, palm));
	static const float share[3] = {0.7f, 1.0f, 0.7f};
	for (int f = 1; f < 5; f++)
		for (int j = 0; j < 3; j++)
		{
			float r[9];
			rotAxis(k, curl * share[j] * rt::PI / 180.0f, r);
			turnNode(b, b.finger[h][f][j], r);
		}
	V kt = norm(cross(st, palm));
	for (int j = 1; j < 3; j++)
	{
		float r[9];
		rotAxis(kt, curl * 0.35f * rt::PI / 180.0f, r);
		turnNode(b, b.finger[h][0][j], r);
	}
}
static void headTurn(const Pose &p, float k, float *out)
{
	float ry[9], rp[9], rr[9], t[9];
	rotAxis(v3(0, 0, 1), p.headYaw * k, ry);
	rotAxis(v3(1, 0, 0), -p.headPitch * k, rp);
	rotAxis(v3(0, 1, 0), p.headRoll * k, rr);
	mm(rp, rr, t);
	mm(ry, t, out);
}
void pose(Body &b, const Pose &p)
{
	if (!b.root) return;
	for (int i = 0; i < kBones; i++)
		if (b.bone[i]) memcpy((float *)(b.bone[i] + 0x34), b.orig[i], 36);
	for (int h = 0; h < 2; h++)
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
				if (b.finger[h][f][j]) memcpy((float *)(b.finger[h][f][j] + 0x34), b.fingerOrig[h][f][j], 36);
	*(float *)(b.bone[HEAD] + 0x64) = b.headScale;
	if (b.fadeNode) { *(float *)(b.root + 0xB4) = 1; *(float *)(b.root + 0xB8) = 1; }
	float *rl = (float *)(b.root + 0x34);
	memcpy(rl, b.fix, 36);
	rl[9] = rl[10] = rl[11] = 0;
	rl[12] = b.scale;
	float rec = p.recline * rt::PI / 180.0f;
	{
		float ln = p.lean * (p.side < 0 ? -1.0f : 1.0f);
		V up = v3(rt::sin(ln) * rt::cos(rec), -rt::sin(rec), rt::cos(ln) * rt::cos(rec));
		aim(b, SPINE, pos(b, SPINE), pos(b, NECK), up);
	}
	aim(b, NECK, pos(b, NECK), pos(b, HEAD), v3(0, 0.09f, 1));
	V h = pos(b, HEAD), want = add(fromf(p.eye), v3(0, -5, -8)), d = sub(want, h);
	rl[9] += d.x; rl[10] += d.y; rl[11] += d.z;
	float pz = pos(b, PELVIS).z;
	if (pz < p.floorZ + 10) rl[11] += p.floorZ + 10 - pz;
	bool headMoves = p.headYaw != 0 || p.headPitch != 0 || p.headRoll != 0;
	if (headMoves) { float r[9]; headTurn(p, 0.4f, r); turn(b, NECK, r); }
	V kl, kr, hl, hr;
	leg(b, L_THIGH, L_CALF, L_FOOT, L_TOE, p, &kl, &hl);
	leg(b, R_THIGH, R_CALF, R_FOOT, R_TOE, p, &kr, &hr);
	V w[2], dir[2], side[2];
	float curl[2];
	if (p.wheel)
	{
		V c = fromf(p.wheelC), a = norm(fromf(p.wheelA));
		V up = norm(sub(v3(0, 0, 1), mul(a, a.z))), right = cross(up, a);
		float lim = p.handsFollow > 0 ? p.handsFollow : 0.8f, turnBy = rt::clamp(p.wheelAngle, -lim, lim);
		for (int h = 0; h < 2; h++)
		{
			float ang = (h == 0 ? 170.0f : 10.0f) * rt::PI / 180.0f + turnBy;
			V radial = add(mul(right, rt::cos(ang)), mul(up, rt::sin(ang)));
			V tangent = add(mul(right, -rt::sin(ang)), mul(up, rt::cos(ang)));
			V grip = add(c, mul(radial, p.wheelR));
			w[h] = add(add(grip, mul(a, 3.5f)), mul(radial, 2.5f));
			dir[h] = norm(sub(mul(a, -1), mul(radial, 0.25f)));
			side[h] = h == 0 ? mul(tangent, -1) : tangent;
			curl[h] = p.curl;
		}
	}
	else
	{
		w[0] = add(add(hl, mul(sub(kl, hl), 0.62f)), v3(1.5f, 0, 4));
		w[1] = add(add(hr, mul(sub(kr, hr), 0.62f)), v3(-1.5f, 0, 4));
		dir[0] = dir[1] = norm(v3(0, 1, -0.3f));
		side[0] = v3(1, 0, 0); side[1] = v3(-1, 0, 0);
		curl[0] = curl[1] = p.curl * 0.35f;
	}
	if (p.lean > 0.01f && p.wheel)
	{
		float s = rt::clamp(p.lean / 0.35f, 0, 1);
		V lap = add(add(hr, mul(sub(kr, hr), 0.62f)), v3(-1.5f, 0, 4));
		w[1] = add(mul(w[1], 1 - s), mul(lap, s));
		dir[1] = norm(add(mul(dir[1], 1 - s), mul(norm(v3(0, 1, -0.3f)), s)));
		side[1] = norm(add(mul(side[1], 1 - s), mul(v3(-1, 0, 0), s)));
		curl[1] = curl[1] * (1 - s) + p.curl * 0.35f * s;
	}
	if (p.shift > 0.001f)
	{
		float s = rt::clamp(p.shift, 0, 1);
		w[1] = add(mul(w[1], 1 - s), mul(add(fromf(p.lever), v3(0, -3, 5)), s));
		dir[1] = norm(add(mul(dir[1], 1 - s), mul(norm(v3(0, 1, -0.5f)), s)));
		side[1] = norm(add(mul(side[1], 1 - s), mul(v3(-1, 0, 0), s)));
	}
	for (int h = 0; h < 2; h++) arm(b, h, w[h], dir[h], side[h], curl[h]);
	if (headMoves) { float r[9]; headTurn(p, 0.6f, r); turn(b, HEAD, r); }
	if (p.hideHead) *(float *)(b.bone[HEAD] + 0x64) = 0.001f;
}

static void legTo(const Body &b, int thigh, int calf, int foot, int toe, V t, V pole)
{
	V j = pos(b, thigh), k0 = pos(b, calf), f0 = pos(b, foot);
	float l1 = len(sub(k0, j)), l2 = len(sub(f0, k0));
	V k = ik(j, t, l1, l2, pole);
	aim(b, thigh, j, k0, sub(k, j));
	V k1 = pos(b, calf), f1 = pos(b, foot);
	aim(b, calf, k1, f1, sub(t, k1));
	if (b.bone[toe])
	{
		V f = pos(b, foot), tt = pos(b, toe);
		aim(b, foot, f, tt, v3(0, 0.9f, -0.2f));
	}
}
static void poseUpright(Body &b, const Free &p);
void poseFree(Body &b, const Free &p)
{
	if (!b.root) return;
	if (rt::absf(p.tilt) < 1e-4f) { poseUpright(b, p); return; }
	float c = rt::cos(p.tilt), s = rt::sin(p.tilt);
	V E = fromf(p.eye);
	auto inv = [&](V v) { return v3(v.x, v.y * c + v.z * s, -v.y * s + v.z * c); };
	auto pt = [&](const float *q, float *o) { V d = inv(sub(fromf(q), E)); o[0] = E.x + d.x; o[1] = E.y + d.y; o[2] = E.z + d.z; };
	auto dir = [&](const float *q, float *o) { V d = inv(fromf(q)); o[0] = d.x; o[1] = d.y; o[2] = d.z; };
	Free q = p;
	for (int k = 0; k < 2; k++)
	{
		pt(p.hand[k], q.hand[k]); dir(p.handDir[k], q.handDir[k]); dir(p.handSide[k], q.handSide[k]);
		pt(p.foot[k], q.foot[k]); dir(p.kneePole[k], q.kneePole[k]);
	}
	poseUpright(b, q);
	float *rl = (float *)(b.root + 0x34);
	float R[9] = {1, 0, 0, 0, c, -s, 0, s, c}, o[9];
	mm(R, rl, o); memcpy(rl, o, 36);
	V t = sub(v3(rl[9], rl[10], rl[11]), E), r = v3(t.x, t.y * c - t.z * s, t.y * s + t.z * c);
	rl[9] = E.x + r.x; rl[10] = E.y + r.y; rl[11] = E.z + r.z;
}
static void poseUpright(Body &b, const Free &p)
{
	if (!b.root) return;
	for (int i = 0; i < kBones; i++)
		if (b.bone[i]) memcpy((float *)(b.bone[i] + 0x34), b.orig[i], 36);
	for (int h = 0; h < 2; h++)
		for (int f = 0; f < 5; f++)
			for (int j = 0; j < 3; j++)
				if (b.finger[h][f][j]) memcpy((float *)(b.finger[h][f][j] + 0x34), b.fingerOrig[h][f][j], 36);
	*(float *)(b.bone[HEAD] + 0x64) = b.headScale;
	if (b.fadeNode) { *(float *)(b.root + 0xB4) = 1; *(float *)(b.root + 0xB8) = 1; }
	float *rl = (float *)(b.root + 0x34);
	memcpy(rl, b.fix, 36);
	rl[9] = rl[10] = rl[11] = 0;
	rl[12] = b.scale;
	aim(b, PELVIS, pos(b, PELVIS), pos(b, SPINE), v3(0, rt::sin(p.lean * 0.3f), rt::cos(p.lean * 0.3f)));
	aim(b, SPINE, pos(b, SPINE), pos(b, NECK), v3(0, rt::sin(p.lean), rt::cos(p.lean)));
	aim(b, NECK, pos(b, NECK), pos(b, HEAD), v3(0, rt::sin(p.lean * 0.4f), 1));
	V h = pos(b, HEAD), want = add(fromf(p.eye), v3(0, -5, -8)), d = sub(want, h);
	rl[9] += d.x; rl[10] += d.y; rl[11] += d.z;
	legTo(b, L_THIGH, L_CALF, L_FOOT, L_TOE, fromf(p.foot[0]), fromf(p.kneePole[0]));
	legTo(b, R_THIGH, R_CALF, R_FOOT, R_TOE, fromf(p.foot[1]), fromf(p.kneePole[1]));
	for (int k = 0; k < 2; k++) arm(b, k, fromf(p.hand[k]), fromf(p.handDir[k]), fromf(p.handSide[k]), p.curl[k]);
	if (p.hideHead) *(float *)(b.bone[HEAD] + 0x64) = 0.001f;
}

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float ease(float u) { u = clampf(u, 0, 1); return u * u * (3 - 2 * u); }
static float seg(float u, float a, float b) { return ease((u - a) / (b - a)); }
static void set3(float *o, float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void lerp3(float *o, const float *a, const float *b, float k) { for (int i = 0; i < 3; i++) o[i] = a[i] + (b[i] - a[i]) * k; }
static float s5(float x) { x = clampf(x, 0, 1); return x * x * x * (x * (x * 6 - 15) + 10); }
static float at5(float x, float a, float b) { return s5((x - a) / (b - a)); }
static const float kHipY = 0, kStandY = 50;
float dropkickStandY() { return kStandY; }
static void dropkickPhases(float t, float u, float G, float *flat, float *s1, float *s2, float *s3)
{
	*flat = at5(t, 0.03f, 0.22f);
	*s1 = u >= 0 ? at5(u, 0.1f * G, 0.48f * G) : 0;
	*s2 = u >= 0 ? at5(u, 0.4f * G, 0.78f * G) : 0;
	*s3 = u >= 0 ? at5(u, 0.7f * G, G) : 0;
}
float dropkickLie(float t, float u, float G) { float f, a, b, c; dropkickPhases(t, u, rt::max(G, 0.4f), &f, &a, &b, &c); return u >= 0 ? 1 - b : f; }
void parkour(Free &F, int kind, float u, float t, const float *eye, float eyeH, float wall, float h, float standY, bool third, const float (*grip)[3], const float *anchor)
{
	memset(&F, 0, sizeof F);
	memcpy(F.eye, eye, 12);
	const float *E = F.eye;
	float feetZ = E[2] - eyeH, W = wall, H = h;
	float nh[2][3], nf[2][3];
	for (int k = 0; k < 2; k++)
	{
		float sx = k ? 1.0f : -1.0f;
		set3(nh[k], E[0] + sx * 18, E[1] + 4, E[2] - 72);
		set3(nf[k], E[0] + sx * 8, E[1] + 1, feetZ + 3);
		set3(F.handDir[k], 0, 0.15f, -1); set3(F.handSide[k], 0, 1, 0); F.curl[k] = 25;
		memcpy(F.hand[k], nh[k], 12); memcpy(F.foot[k], nf[k], 12);
		set3(F.kneePole[k], sx * 0.2f, 1, 0.3f);
	}
	F.hideHead = !third;
	if (kind == 2 || kind == 3)
	{
		float kin = seg(u, 0, 0.2f), kout = seg(u, 0.82f, 1.0f);
		for (int k = 0; k < 2; k++)
		{
			float sx = k ? 1.0f : -1.0f, g[3], up[3];
			set3(g, sx * 10, W + 1.5f, H + 5);
			if (grip) set3(g, grip[k][0], grip[k][1] - 0.5f, grip[k][2] + 4);
			lerp3(up, nh[k], g, kin);
			up[2] += 12 * rt::sin(rt::PI * kin) * (1 - kin * 0.5f);
			lerp3(F.hand[k], up, nh[k], kout);
			float gd[3] = {0, 1, -0.9f}, gs[3] = {-sx, 0, 0};
			lerp3(F.handDir[k], F.handDir[k], gd, kin * (1 - kout));
			lerp3(F.handSide[k], F.handSide[k], gs, kin * (1 - kout));
			F.curl[k] = 25 + 35 * kin * (1 - kout);
		}
		F.lean = 0.55f * rt::sin(rt::PI * clampf((u - 0.3f) / 0.7f, 0, 1));
		float lw[3], rw[3], rt0[3], ls[3], rs[3];
		set3(lw, -7, rt::min(W - 6, E[1] + 6), feetZ + 6);
		set3(rw, 7, rt::min(W - 5, E[1] + 6), feetZ + 30);
		set3(rt0, 6, W + 16, H + 4);
		set3(ls, -7, standY, H + 2); set3(rs, 8, standY, H + 2);
		float kwall = seg(u, 0.08f, 0.3f), kswing = seg(u, 0.45f, 0.75f), kstandL = seg(u, 0.72f, 0.95f), kstandR = seg(u, 0.85f, 1.0f);
		float tmp[3];
		lerp3(tmp, nf[0], lw, kwall); lerp3(F.foot[0], tmp, ls, kstandL);
		lerp3(tmp, nf[1], rw, kwall); float tmp2[3]; lerp3(tmp2, tmp, rt0, kswing); tmp2[2] += 18 * rt::sin(rt::PI * kswing);
		lerp3(F.foot[1], tmp2, rs, kstandR);
		float wallPole[3] = {0, 1, 0.3f}, overPole[3] = {0.3f, 1, 0.7f};
		lerp3(F.kneePole[1], wallPole, overPole, kswing);
		lerp3(F.kneePole[0], wallPole, overPole, kstandL);
	}
	else if (kind == 1)
	{
		float kin = seg(u, 0, 0.25f), kout = seg(u, 0.7f, 0.95f), g[3];
		set3(g, -8, W + 10, H + 5);
		if (grip) set3(g, grip[0][0], grip[0][1] + 6, grip[0][2] + 4);
		float tmp[3]; lerp3(tmp, nh[0], g, kin); lerp3(F.hand[0], tmp, nh[0], kout);
		float gd[3] = {0, 0.3f, -1}, gs[3] = {1, 0, 0};
		lerp3(F.handDir[0], F.handDir[0], gd, kin * (1 - kout)); lerp3(F.handSide[0], F.handSide[0], gs, kin * (1 - kout));
		F.curl[0] = 25 + 20 * kin * (1 - kout);
		set3(F.hand[1], E[0] + 26, E[1] + 6, E[2] - 50);
		float k = rt::sin(rt::PI * u);
		for (int i = 0; i < 2; i++) { F.foot[i][0] += 8 * k; F.foot[i][1] += 26 * k; F.foot[i][2] += 30 * k; set3(F.kneePole[i], 0.3f, 1, 0.8f); }
		F.lean = 0.4f * k;
	}
	else if (kind == 6)
	{
		float G = rt::max(wall, 0.4f), PI = 3.14159265f;
		float C[3];
		if (anchor) memcpy(C, anchor, 12); else set3(C, eye[0], eye[1], eye[2] - eyeH);
		float flat, s1, s2, s3;
		dropkickPhases(t, u, G, &flat, &s1, &s2, &s3);
		bool ground = u >= 0;
		float up = eyeH - 64;
		float th, Hy, Hz;
		float whip = ground ? 0 : at5(t, 0.12f, 0.2f) * (1 - at5(t, 0.2f, 0.42f));
		float comp = standY >= 0 ? at5(standY, 0, 0.04f) * (1 - at5(standY, 0.04f, 0.2f)) : 0;
		auto hipsAt = [&](bool gr, float fl, float a1, float a2, float a3, float *oth, float *oy, float *oz) {
			if (!gr) { *oth = h * fl + 0.14f * whip + 0.1f * comp; *oy = kHipY * fl; *oz = 64 + (10 - 64) * fl; return; }
			float bounce = 2.5f * rt::sin(PI * clampf(u / 0.14f, 0, 1));
			float a = h + (30 * PI / 180 - h) * a1; a = a + (-22 * PI / 180 - a) * a2; a = a * (1 - a3);
			float y = kHipY + 30 * a2; y = y + (kStandY - 5 - y) * a3;
			float z = 10 + bounce * (1 - a1); z = z + (36 - z) * a2; z = z + (64 - z) * a3;
			*oth = a; *oy = y; *oz = z;
		};
		hipsAt(ground, flat, s1, s2, s3, &th, &Hy, &Hz);
		auto eyeOf = [&](float a, float y, float z, float *o) { float c = rt::cos(a), sn = rt::sin(a); set3(o, C[0], C[1] + y + 5 * c - up * sn, C[2] + z + 5 * sn + up * c); };
		eyeOf(th, Hy, Hz, F.eye);
		const float *Ev = F.eye;
		auto airPose = [&](const float *Eb, float a, float tt, float (*hand)[3], float (*hdir)[3], float (*hside)[3], float (*foot)[3], float (*pole)[3], float *curl) {
			float fz = Eb[2] - eyeH;
			for (int k = 0; k < 2; k++)
			{
				float sx = k ? 1.0f : -1.0f, lag = k ? 0.018f : 0;
				float ka = at5(tt - lag, 0, 0.09f), kb = at5(tt - lag, 0.05f, 0.17f);
				float nf0[3], tf[3], ef[3], p[3];
				set3(nf0, Eb[0] + sx * 8, Eb[1] + 1, fz + 3);
				set3(tf, Eb[0] + sx * 9, Eb[1] + 14, fz + 42);
				set3(ef, Eb[0] + sx * 4.5f, Eb[1] + 2, fz - 1 + (k ? 0.0f : 1.5f));
				lerp3(p, nf0, tf, ka); lerp3(foot[k], p, ef, kb);
				foot[k][1] += 4 * comp; foot[k][2] += 16 * comp;
				float p0[3] = {sx * 0.2f, 1, 0.3f}, pt[3] = {sx * 0.15f, 1, 0.5f}, pe[3] = {sx * 0.05f, 1, 0}, pn[3];
				lerp3(pn, p0, pt, ka); lerp3(pole[k], pn, pe, kb);
				float ha = at5(tt, 0, 0.1f), hb = at5(tt, 0.06f, 0.24f);
				float nh0[3], wb[3], ex[3], hh[3];
				set3(nh0, Eb[0] + sx * 18, Eb[1] + 4, Eb[2] - 72);
				set3(wb, Eb[0] + sx * 22, Eb[1] - 22, Eb[2] - 54);
				set3(ex, Eb[0] + sx * 42, Eb[1] - 2, Eb[2] - 44);
				lerp3(hh, nh0, wb, ha); lerp3(hand[k], hh, ex, hb);
				float d0[3] = {0, 0.15f, -1}, dOut[3] = {sx, 0.1f, -0.2f}, s0[3] = {0, 1, 0};
				lerp3(hdir[k], d0, dOut, hb); memcpy(hside[k], s0, 12);
				curl[k] = 25 - 15 * hb;
			}
			float c = rt::cos(a), sn = rt::sin(a);
			auto fwd = [&](float *q, bool point) {
				float y = q[1] - (point ? Eb[1] : 0), z = q[2] - (point ? Eb[2] : 0);
				q[1] = (point ? Eb[1] : 0) + y * c - z * sn; q[2] = (point ? Eb[2] : 0) + y * sn + z * c;
			};
			for (int k = 0; k < 2; k++) { fwd(hand[k], true); fwd(foot[k], true); fwd(hdir[k], false); fwd(hside[k], false); fwd(pole[k], false); }
		};
		if (!ground) airPose(Ev, th, t, F.hand, F.handDir, F.handSide, F.foot, F.kneePole, F.curl);
		else
		{
			float le[3], lth, ly, lz;
			hipsAt(false, 1, 0, 0, 0, &lth, &ly, &lz);
			eyeOf(lth, ly, lz, le);
			float tl = rt::max(t - u, 0.35f);
			airPose(le, lth, tl, F.hand, F.handDir, F.handSide, F.foot, F.kneePole, F.curl);
			float bounce = 3 * rt::sin(PI * clampf(u / 0.16f, 0, 1));
			for (int k = 0; k < 2; k++)
			{
				float sx = k ? 1.0f : -1.0f, lag = k ? 0.04f * G : 0;
				float k1 = at5(u - lag, 0.1f * G, 0.46f * G), k3 = at5(u - lag, 0.72f * G, G);
				F.foot[k][2] += bounce * (1 - k1);
				float fi[3], fs[3], p[3];
				set3(fi, C[0] + sx * 10, C[1] + kHipY + 44 + (k ? 4.0f : 0.0f), C[2] + 3);
				set3(fs, C[0] + sx * 8, C[1] + kStandY + 1, C[2] + 3);
				lerp3(p, F.foot[k], fi, k1); lerp3(F.foot[k], p, fs, k3);
				float pu[3] = {sx * 0.25f, 0.3f, 1}, pst[3] = {sx * 0.2f, 1, 0.3f}, pn[3];
				lerp3(pn, F.kneePole[k], pu, k1); lerp3(F.kneePole[k], pn, pst, k3);
				float hp[3], hc[3], hs[3], q[3];
				set3(hp, C[0] + sx * 20, C[1] + kHipY - 14, C[2] + 3);
				if (k == 0) set3(hc, C[0] - 16, C[1] + kHipY + 40, C[2] + 4); else set3(hc, C[0] + 16, C[1] + kHipY + 60, C[2] + 46);
				set3(hs, Ev[0] + sx * 18, Ev[1] + 4, Ev[2] - 72);
				lerp3(q, F.hand[k], hp, s1); lerp3(q, q, hc, s2); lerp3(F.hand[k], q, hs, s3);
				float dp[3] = {0, -0.5f, -0.85f}, dc[3] = {0, 0.4f, k ? -0.2f : -0.9f}, ds[3] = {0, 0.15f, -1}, sp[3] = {sx, 0, 0}, ss[3] = {0, 1, 0}, dq[3];
				lerp3(dq, F.handDir[k], dp, s1); lerp3(dq, dq, dc, s2); lerp3(F.handDir[k], dq, ds, s3);
				lerp3(dq, F.handSide[k], sp, s1); lerp3(F.handSide[k], dq, ss, s3);
				F.curl[k] = F.curl[k] + (8 - F.curl[k]) * s1; F.curl[k] = F.curl[k] + (25 - F.curl[k]) * s3;
			}
		}
		F.tilt = th;
		F.lean = (ground ? 0.12f * s2 * (1 - s3) : 0.18f * at5(t, 0, 0.12f) * (1 - flat)) - 0.05f * flat * (1 - s1);
	}
	else
	{
		float kt = clampf(t / 0.25f, 0, 1), tuck = seg(t, 0.15f, 0.45f);
		if (kind == 4)
		{
			float wf[3]; set3(wf, 6, rt::min(W - 4, E[1] + 10), feetZ + 28);
			if (kt < 1) memcpy(F.foot[1], wf, 12);
			for (int k = 0; k < 2; k++) { float sx = k ? 1.0f : -1.0f; set3(F.hand[k], E[0] + sx * 14, E[1] + 16, E[2] + 4 - 20 * tuck); set3(F.handDir[k], 0, 0.3f, 1); set3(F.handSide[k], -sx, 0, 0); F.curl[k] = 15; }
		}
		else
			for (int k = 0; k < 2; k++) { float sx = k ? 1.0f : -1.0f; set3(F.hand[k], E[0] + sx * 32, E[1] + 6, E[2] - 28); set3(F.handDir[k], sx, 0.2f, -0.3f); set3(F.handSide[k], 0, 1, 0); }
		for (int k = 0; k < 2; k++) { F.foot[k][1] += 6 * tuck; F.foot[k][2] += 26 * tuck; set3(F.kneePole[k], (k ? 0.2f : -0.2f), 1, 0.5f); }
		F.lean = 0.25f * tuck;
	}
}

void parkourEye(int kind, float u, float eyeH, float gap, float wall, float h, float standY, float *y, float *z)
{
	float ts[5], ys[5], zs[5];
	int n;
	if (kind == 1)
	{
		n = 3;
		ts[0] = 0; ts[1] = 0.5f; ts[2] = 1;
		ys[0] = 0; ys[1] = rt::max(0.0f, wall - 6); ys[2] = standY;
		zs[0] = eyeH; zs[1] = rt::max(eyeH + 8, h + eyeH - 12); zs[2] = h + 2 + eyeH;
	}
	else
	{
		n = 5;
		ts[0] = 0; ts[1] = 0.35f; ts[2] = 0.6f; ts[3] = 0.82f; ts[4] = 1;
		ys[0] = 0; ys[1] = gap; ys[2] = rt::max(gap, wall - 10); ys[3] = rt::max(gap, wall - 4); ys[4] = rt::max(standY, wall + 14);
		zs[0] = eyeH; zs[1] = rt::max(h + 20, eyeH - 10); zs[2] = rt::max(h + 50, zs[1]); zs[3] = rt::max(h + 72, zs[2]); zs[4] = h + 2 + eyeH;
	}
	u = clampf(u, 0, 1);
	int i = 0;
	while (i < n - 2 && u > ts[i + 1]) i++;
	float k = ease((u - ts[i]) / (ts[i + 1] - ts[i]));
	*y = ys[i] + (ys[i + 1] - ys[i]) * k;
	*z = zs[i] + (zs[i + 1] - zs[i]) * k;
}

void parkourLook(const Free &F, int kind, float u, float *target, float *w)
{
	float hands[3] = {(F.hand[0][0] + F.hand[1][0]) * 0.5f, (F.hand[0][1] + F.hand[1][1]) * 0.5f, (F.hand[0][2] + F.hand[1][2]) * 0.5f};
	float knee[3] = {F.foot[1][0] * 0.7f, F.foot[1][1], F.foot[1][2] + 18};
	if (kind == 1) { memcpy(hands, F.hand[0], 12); knee[0] = (F.foot[0][0] + F.foot[1][0]) * 0.5f; knee[1] = (F.foot[0][1] + F.foot[1][1]) * 0.5f; knee[2] = (F.foot[0][2] + F.foot[1][2]) * 0.5f + 10; }
	float k = kind == 1 ? seg(u, 0.35f, 0.55f) : seg(u, 0.45f, 0.62f);
	lerp3(target, hands, knee, k);
	*w = kind == 1 ? seg(u, 0, 0.12f) * (1 - seg(u, 0.75f, 1)) : (kind == 2 || kind == 3) ? seg(u, 0, 0.15f) * (1 - seg(u, 0.8f, 1)) : 0;
}
}
