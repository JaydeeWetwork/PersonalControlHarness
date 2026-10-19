// Camera module self-tests. Fakes only: a scripted target and scripted
// worlds, no engine world, no renderer. Deterministic (fixed dt steps).
#include "litholattice/camera/camera_selftest.h"

#include <cmath>
#include <cstdio>
#include <memory>

#include "litholattice/camera/camera.h"
#include "litholattice/console/concmd.h"

namespace litholattice::camera {
namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_fails;
}
bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float vlen(Vec3 a) { return std::sqrt(dot(a, a)); }
bool vnear(Vec3 a, Vec3 b, float eps = 1e-3f) {
  return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
Vec3 xfp(const Mat4& m, Vec3 v) {  // point transform, no divide (affine)
  return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z + m.m[12],
          m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z + m.m[13],
          m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14]};
}
float clipZ(const Mat4& m, Vec3 v) {  // NDC z after the perspective divide
  const float z = m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z + m.m[14];
  const float w = m.m[3] * v.x + m.m[7] * v.y + m.m[11] * v.z + m.m[15];
  return z / w;
}

struct FakeTarget final : ICameraTarget {
  Vec3 anchor{0, 64, 0};
  float yaw = 0, pitch = 0;
  Vec3 vel{};
  Vec3 anchorPosition() const override { return anchor; }
  float facingYaw() const override { return yaw; }
  float facingPitch() const override { return pitch; }
  Vec3 velocity() const override { return vel; }
};

struct DistWorld final : ICameraWorld {
  float hitDist = -1.f;  // < 0 = open sky
  CameraRayHit castRay(Vec3, Vec3, float maxDist) const override {
    if (hitDist >= 0.f && hitDist <= maxDist) return {true, hitDist};
    return {};
  }
};

void settle(ICamera& cam, const ICameraWorld& w, int frames = 300) {
  for (int i = 0; i < frames; ++i) cam.update(1.f / 60.f, w);
}

void testMatrices() {
  std::printf("camera: matrices\n");
  CameraPose p;
  p.position = {1, 2, 3};
  p.forward = {1, 0, 0};
  p.up = {0, 1, 0};
  p.fovDeg = 70;
  const Mat4 view = cameraViewMatrix(p);
  check(vnear(xfp(view, p.position), {0, 0, 0}), "view maps the eye to the origin");
  check(vnear(xfp(view, {6, 2, 3}), {0, 0, -5}), "view maps 5 ahead to -Z 5");
  check(vnear(xfp(view, {1, 7, 3}), {0, 5, 0}), "view maps 5 up to +Y 5");
  const Mat4 vp = mul(cameraProjectionMatrix(70, 1.f, 0.05f, 1000.f), view);
  check(near(clipZ(vp, {1.05f, 2, 3}), -1.f, 1e-2f), "projection: near plane -> NDC z -1");
  check(near(clipZ(vp, {1001, 2, 3}), 1.f, 1e-2f), "projection: far plane -> NDC z +1");
  // FOV clamps to the house [10, 120] degrees.
  const Mat4 wide = cameraProjectionMatrix(500.f, 1.f, 0.05f, 1000.f);
  check(near(wide.m[5], 1.f / std::tan(60.f * 3.14159265f / 180.f), 1e-3f), "FOV clamps at 120");
  const Mat4 narrow = cameraProjectionMatrix(1.f, 1.f, 0.05f, 1000.f);
  check(near(narrow.m[5], 1.f / std::tan(5.f * 3.14159265f / 180.f), 1e-2f), "FOV clamps at 10");
}

void testFirstPerson() {
  std::printf("camera: first person\n");
  auto target = std::make_shared<FakeTarget>();
  target->anchor = {10, 64, 10};
  Camera cam;
  cam.setMode(CameraMode::FirstPerson);
  cam.attach(target);
  NullCameraWorld world;
  cam.update(1.f / 60.f, world);
  const CameraPose p = cam.pose();
  check(vnear(p.position, {10, 65.625f, 10}), "FP eye = anchor + eye offset");
  check(vnear(p.forward, {1, 0, 0}), "FP forward follows target facing (yaw 0)");
  check(vnear(p.up, {0, 1, 0}, 1e-4f), "FP up is world up when level");
  check(near(p.fovDeg, 70.f, 0.5f), "FP fov settles at the base 70");
  target->yaw = 1.5707963f;  // +90 deg: forward becomes +Z
  cam.update(1.f / 60.f, world);
  check(vnear(cam.pose().forward, {0, 0, 1}, 1e-3f), "FP tracks a target turn");
  cam.addLookInput(1.5707963f, 0.f);  // camera-side offset rides on top
  cam.update(1.f / 60.f, world);
  check(vnear(cam.pose().forward, {-1, 0, 0}, 1e-3f), "FP look input offsets the facing");
  check(cam.chunkLoadRadiusChunks() == 4, "chunk radius defaults to 4 (JD budget)");
  check(vnear(cam.chunkLoadCenter(), cam.pose().position), "chunk load center is the camera");
  // FOV kick: sprint-speed target widens the FOV, damped.
  target->vel = {60, 0, 0};
  settle(cam, world);
  check(cam.pose().fovDeg > 76.f, "FOV kick widens at sprint speed");
  target->vel = {};
}

void testThirdPerson() {
  std::printf("camera: third person orbit + spring arm\n");
  auto target = std::make_shared<FakeTarget>();
  target->anchor = {0, 64, 0};
  Camera cam;
  cam.setMode(CameraMode::ThirdPerson);
  cam.attach(target);
  NullCameraWorld world;
  settle(cam, world);
  const Vec3 pivot{0, 65.5f, 0};  // anchor + pivotOffset
  const Vec3 pos = cam.pose().position;
  // Boom length measured to the shouldered pivot: anchor + pivot + shoulder.
  const Vec3 shouldered = pivot + Vec3{0, 0.25f, 0.9f};
  check(near(vlen(sub(pos, shouldered)), 6.f, 0.05f), "boom rests at the default distance 6");
  check(pos.x < -5.5f, "camera sits behind the target's facing (+X)");
  check(vlen(sub(cam.pose().forward, Vec3{1, 0, 0})) < 0.25f, "camera looks at the target");
  // Pitch clamp: huge look-up input stops at orbitPitchMax.
  cam.addLookInput(0.f, 100.f);
  settle(cam, world);
  const Vec3 pos2 = cam.pose().position;
  check(pos2.y < shouldered.y - 4.f, "pitch clamps: camera drops low looking up, never flips");
  check(near(vlen(sub(pos2, shouldered)), 6.f, 0.05f), "boom length survives the pitch clamp");
  cam.addLookInput(0.f, -100.f);  // back to level-ish for the zoom checks
  settle(cam, world);
  cam.addZoomInput(100.f);  // dolly in hard
  settle(cam, world);
  check(near(vlen(sub(cam.pose().position, shouldered)), 1.f, 0.05f), "zoom clamps at min distance 1");
  cam.addZoomInput(-100.f);  // dolly out hard
  settle(cam, world);
  check(near(vlen(sub(cam.pose().position, shouldered)), 16.f, 0.1f), "zoom clamps at max distance 16");

  // Spring arm: a wall 3 blocks behind the pivot pulls the camera in
  // instantly (minus the probe radius), then it eases back out.
  DistWorld wall;
  wall.hitDist = 3.f;
  Camera cam2;
  cam2.setMode(CameraMode::ThirdPerson);
  cam2.attach(target);
  settle(cam2, world);
  cam2.update(1.f / 60.f, wall);  // ONE frame against the wall
  check(near(vlen(sub(cam2.pose().position, shouldered)), 2.7f, 0.02f),
        "spring arm pulls in instantly to hit - probe (3 - 0.3)");
  settle(cam2, world, 240);  // wall gone: ease back out over ~4 s
  check(vlen(sub(cam2.pose().position, shouldered)) > 5.8f, "spring arm eases back out");
}

void testFollowDamping() {
  std::printf("camera: follow damping (frame-rate independence)\n");
  auto target = std::make_shared<FakeTarget>();
  target->anchor = {0, 64, 0};
  NullCameraWorld world;
  auto runChase = [&](float dt, int frames) {
    Camera cam;
    cam.setMode(CameraMode::Follow);
    cam.attach(target);
    cam.update(dt, world);  // seed behind the target at the origin
    target->anchor = {20, 64, 0};  // target teleports; the chase must converge
    for (int i = 0; i < frames; ++i) cam.update(dt, world);
    return cam.pose().position;
  };
  const Vec3 a = runChase(1.f / 30.f, 60);    // 2 s at 30 fps
  target->anchor = {0, 64, 0};
  const Vec3 b = runChase(1.f / 120.f, 240);  // 2 s at 120 fps
  check(vnear(a, b, 0.1f), "30 fps and 120 fps chases land together (exp damping)");
  check(vnear(a, {12, 67, 0}, 0.75f), "chase converges behind/above the new anchor");
  target->anchor = {0, 64, 0};
}

void testShake() {
  std::printf("camera: trauma shake\n");
  auto target = std::make_shared<FakeTarget>();
  target->anchor = {0, 64, 0};
  Camera cam;
  cam.setMode(CameraMode::FirstPerson);
  cam.attach(target);
  NullCameraWorld world;
  cam.update(1.f / 60.f, world);
  const Vec3 baseFwd = cam.pose().forward;
  const Vec3 basePos = cam.pose().position;
  cam.addTrauma(0.5f);
  cam.addTrauma(0.7f);
  check(near(cam.trauma(), 1.f), "trauma stacks and clamps at 1");
  bool moved = false;
  float maxAngle = 0;
  for (int i = 0; i < 30; ++i) {  // 0.5 s of shake
    cam.update(1.f / 60.f, world);
    const Vec3 f = cam.pose().forward;
    const float ang = std::acos(std::min(1.f, dot(f, baseFwd)));
    maxAngle = std::max(maxAngle, ang);
    if (!vnear(f, baseFwd, 1e-4f) || !vnear(cam.pose().position, basePos, 1e-4f)) moved = true;
  }
  check(moved, "shake visibly offsets the pose");
  check(maxAngle < 0.08f, "shake stays rotational and small (< ~4.6 deg)");
  for (int i = 0; i < 300 && cam.trauma() > 0.f; ++i) cam.update(1.f / 60.f, world);
  check(near(cam.trauma(), 0.f), "trauma decays linearly to zero");
  cam.update(1.f / 60.f, world);
  check(vnear(cam.pose().forward, baseFwd, 1e-4f), "pose returns exactly to base after decay");
  check(vnear(cam.pose().position, basePos, 1e-4f), "no positional drift from shake");
}

void testManagerBlend() {
  std::printf("camera: manager blending\n");
  auto camA = std::make_shared<Camera>();
  camA->setMode(CameraMode::Fixed);
  CameraPose pa;
  pa.position = {0, 64, 0};
  pa.forward = {1, 0, 0};
  pa.up = {0, 1, 0};
  camA->setPose(pa);
  auto camB = std::make_shared<Camera>();
  camB->setMode(CameraMode::Fixed);
  camB->config().fovDeg = 90.f;  // output FOV comes from the lens config
  CameraPose pb = pa;
  pb.position = {10, 64, 0};
  camB->setPose(pb);

  CameraManager mgr;
  NullCameraWorld world;
  check(!mgr.addCamera("", camA), "manager rejects an empty name");
  check(mgr.addCamera("a", camA), "manager adds camera a");
  check(mgr.addCamera("b", camB), "manager adds camera b");
  check(mgr.activeName() == "a", "first camera is live by default");
  check(!mgr.setActive("nope"), "setActive on an unknown camera fails");
  check(mgr.setActive("b", 1.f, CameraBlendEase::Linear), "setActive b with a 1 s linear blend");
  mgr.update(0.5f, world);
  check(mgr.blending(), "manager reports blending mid-blend");
  check(near(mgr.pose().position.x, 5.f, 0.05f), "blend midpoint is halfway (linear)");
  check(near(mgr.pose().fovDeg, 80.f, 1.f), "blend interpolates FOV");
  mgr.update(0.5f, world);
  check(!mgr.blending(), "blend completes");
  check(near(mgr.pose().position.x, 10.f, 1e-4f), "output lands exactly on camera b");
  check(mgr.chunkLoadRadiusChunks() == 4, "manager forwards the active chunk radius");
  check(near(cameraBlendFactor(CameraBlendEase::EaseInOut, 0.5f), 0.5f), "ease-in-out midpoint is 0.5");
  check(near(cameraBlendFactor(CameraBlendEase::EaseIn, 0.f), 0.f) &&
            near(cameraBlendFactor(CameraBlendEase::HardOut, 1.f), 1.f),
        "easing endpoints are exact");
}

void testFrustum() {
  std::printf("camera: frustum\n");
  CameraPose p;
  p.position = {0, 64, 0};
  p.forward = {1, 0, 0};
  p.up = {0, 1, 0};
  p.fovDeg = 70;
  const Mat4 vp = mul(cameraProjectionMatrix(70, 1.f, 0.05f, 1000.f), cameraViewMatrix(p));
  const Frustum fr = extractFrustum(vp);
  check(fr.containsPoint({10, 64, 0}), "point ahead is inside");
  check(!fr.containsPoint({-5, 64, 0}), "point behind is outside");
  check(fr.intersectsAabb({9, 63, -1}, {11, 65, 1}), "AABB ahead intersects");
  check(!fr.intersectsAabb({-11, 63, -1}, {-9, 65, 1}), "AABB behind is excluded");
  check(!fr.intersectsAabb({9, 120, -1}, {11, 122, 1}), "AABB far above the cone is excluded");
  // Agreement with the core renderer's test on the same matrix.
  check(aabbInFrustum(vp, {9, 63, -1}, {11, 65, 1}) == fr.intersectsAabb({9, 63, -1}, {11, 65, 1}) &&
            aabbInFrustum(vp, {-11, 63, -1}, {-9, 65, 1}) == fr.intersectsAabb({-11, 63, -1}, {-9, 65, 1}),
        "frustum agrees with core aabbInFrustum");
}

void testLifetime() {
  std::printf("camera: attach/detach lifetime\n");
  Camera cam;
  cam.setMode(CameraMode::FirstPerson);
  NullCameraWorld world;
  {
    auto target = std::make_shared<FakeTarget>();
    target->anchor = {3, 64, 3};
    cam.attach(target);
    check(cam.hasTarget(), "attached target reports live");
    cam.update(1.f / 60.f, world);
  }  // target destroyed here
  check(!cam.hasTarget(), "expired target reports dead (no dangling)");
  const Vec3 held = cam.pose().position;
  cam.update(1.f / 60.f, world);  // must not crash or jump
  check(vnear(cam.pose().position, held, 1e-4f), "pose holds after target death");
  check(vnear(held, {3, 65.625f, 3}), "held pose is the last eye position");
  cam.detach();
  check(!cam.hasTarget(), "detach clears the target");
  std::weak_ptr<FakeTarget> dead;
  {
    auto t2 = std::make_shared<FakeTarget>();
    dead = t2;
  }
  cam.attach(dead);
  check(!cam.hasTarget(), "attaching an expired weak_ptr is a detach");
}

void testCvars() {
  std::printf("camera: cvars\n");
  console::ConsoleRegistry reg;
  std::string err;
  check(registerCameraCvars(reg, &err), "camera cvars register");
  check(!registerCameraCvars(reg, &err), "duplicate registration fails cleanly");
  const CameraCvarValues v = readCameraCvars(reg);
  check(v.chunkRadiusChunks == 4 && near(v.defaultFovDeg, 70.f) && near(v.thirdPersonDistance, 6.f) &&
            near(v.shakeScale, 1.f) && !v.headBob && v.fovKick,
        "cvar defaults match the JD budget + house knobs");
  check(reg.setCVar("camera.chunk_radius", "8", &err), "chunk radius is settable");
  check(readCameraCvars(reg).chunkRadiusChunks == 8, "chunk radius reads back");
  CameraConfig cfg;
  applyCameraCvars(readCameraCvars(reg), cfg);
  check(cfg.chunkRadiusChunks == 8, "apply syncs the chunk radius into a config");
}

}  // namespace

int runCameraSelfTest() {
  std::printf("== camera selftest ==\n");
  testMatrices();
  testFirstPerson();
  testThirdPerson();
  testFollowDamping();
  testShake();
  testManagerBlend();
  testFrustum();
  testLifetime();
  testCvars();
  std::printf("camera selftest: %s (%d failure%s)\n", g_fails ? "FAILED" : "OK", g_fails,
              g_fails == 1 ? "" : "s");
  return g_fails;
}

}  // namespace litholattice::camera
