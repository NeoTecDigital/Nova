// Written by Richard Christopher, Copyright 2026 NeoTec Digital
#include "Core/math/spatial_cluster.h"
#include "Core/math/input_filter.h"
#include "Core/math/engine_physics.h"
#include <iostream>
#include <chrono>
#include <vector>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <random>

// ---------------------------------------------------------------------------
// Assertion harness. Deliberately NOT <cassert>: assert() is compiled out under
// -DNDEBUG, which would silently turn every check below into a no-op.
// ---------------------------------------------------------------------------
static int g_failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg);  \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_NEAR(actual, expected, tol, msg)                                 \
    do {                                                                       \
        const double actual_ = static_cast<double>(actual);                    \
        const double expected_ = static_cast<double>(expected);                \
        if (!(std::fabs(actual_ - expected_) <= (tol))) {                      \
            fprintf(stderr, "  [FAIL] %s:%d: %s (got %.9f, expected %.9f)\n",  \
                    __FILE__, __LINE__, msg, actual_, expected_);              \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

namespace {

constexpr float kUnitBoxHalfExtent = 1.0f;

Nova::Math::ClusterAABB makeUnitBox() {
    return Nova::Math::ClusterAABB{glm::vec3(-kUnitBoxHalfExtent), glm::vec3(kUnitBoxHalfExtent)};
}

Nova::Math::ClusterAABB makeBox(const glm::vec3& center, float half) {
    return Nova::Math::ClusterAABB{center - glm::vec3(half), center + glm::vec3(half)};
}

// ---------------------------------------------------------------------------
// Narrowphase ray/AABB correctness.
// ---------------------------------------------------------------------------
void testRayAabbHitMiss() {
    const Nova::Math::ClusterAABB box = makeUnitBox();
    float t_min = 0.0f;
    float t_max = 0.0f;

    // Head-on hit: origin 10 units out on +Z, box front face at z = +1 => t = 9.
    Nova::Math::Ray3D head_on(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(box.intersectRay(head_on, t_min, t_max), "head-on ray must hit the unit box");
    CHECK_NEAR(t_min, 9.0, 1e-4, "entry distance for head-on ray");
    CHECK_NEAR(t_max, 11.0, 1e-4, "exit distance for head-on ray");
    const glm::vec3 entry = head_on.getPoint(t_min);
    CHECK_NEAR(entry.z, kUnitBoxHalfExtent, 1e-4, "entry point lies on the +Z face");

    // Same ray reversed: the box is entirely behind the origin => miss.
    Nova::Math::Ray3D pointing_away(glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(!box.intersectRay(pointing_away, t_min, t_max), "ray pointing away must miss");

    // Laterally displaced ray: parallel to the box but outside every slab.
    Nova::Math::Ray3D offset(glm::vec3(5.0f, 5.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(!box.intersectRay(offset, t_min, t_max), "laterally offset ray must miss");

    // Ray originating inside the box: entry parameter is negative, still a hit.
    Nova::Math::Ray3D inside(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(box.intersectRay(inside, t_min, t_max), "ray starting inside the box must hit");
    CHECK_NEAR(t_min, -1.0, 1e-4, "interior ray entry parameter is behind the origin");
    CHECK_NEAR(t_max, 1.0, 1e-4, "interior ray exit parameter");
}

// ---------------------------------------------------------------------------
// Grazing behaviour. ClusterAABB::intersectRay substitutes 1e-7 for a zero
// direction component, so a ray travelling exactly along a face plane produces
// a degenerate slab of [.., 0] and resolves as a MISS. The boundary is
// therefore exclusive: 1 - eps hits, exactly 1 misses, 1 + eps misses.
// This is asserted so the convention cannot silently flip.
// ---------------------------------------------------------------------------
void testRayAabbGraze() {
    const Nova::Math::ClusterAABB box = makeUnitBox();
    float t_min = 0.0f;
    float t_max = 0.0f;

    Nova::Math::Ray3D on_face(glm::vec3(kUnitBoxHalfExtent, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(!box.intersectRay(on_face, t_min, t_max),
          "ray grazing exactly along the +X face plane resolves as a miss");

    Nova::Math::Ray3D just_inside(glm::vec3(kUnitBoxHalfExtent - 1e-3f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(box.intersectRay(just_inside, t_min, t_max), "ray just inside the +X face must hit");
    CHECK_NEAR(t_min, 9.0, 1e-4, "grazing-inside entry distance matches the head-on case");

    Nova::Math::Ray3D just_outside(glm::vec3(kUnitBoxHalfExtent + 1e-3f, 0.0f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(!box.intersectRay(just_outside, t_min, t_max), "ray just outside the +X face must miss");

    Nova::Math::Ray3D corner(glm::vec3(kUnitBoxHalfExtent, kUnitBoxHalfExtent, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(!box.intersectRay(corner, t_min, t_max),
          "ray grazing exactly along a corner edge resolves as a miss, consistent with the face case");
}

// ---------------------------------------------------------------------------
// Broadphase must never drop a true positive: every AABB the narrowphase
// confirms has to be present in the candidate set returned by queryRay.
// ---------------------------------------------------------------------------
void testBroadphaseContainsNarrowphaseHits(const std::vector<Nova::Math::ClusterAABB>& boxes,
                                           const Nova::Math::SpatialClusterIndex& index,
                                           const Nova::Math::Ray3D& ray,
                                           const char* label) {
    uint32_t tests_performed = 0;
    std::vector<uint32_t> candidates = index.queryRay(ray, tests_performed);
    CHECK(tests_performed > 0, "broadphase must test at least one cluster");

    std::vector<uint32_t> true_hits;
    for (uint32_t i = 0; i < boxes.size(); ++i) {
        float t_min = 0.0f;
        float t_max = 0.0f;
        if (boxes[i].intersectRay(ray, t_min, t_max)) {
            true_hits.push_back(i);
        }
    }
    CHECK(!true_hits.empty(), label);

    for (uint32_t id : true_hits) {
        const bool present = std::binary_search(candidates.begin(), candidates.end(), id);
        CHECK(present, "broadphase dropped an AABB that the narrowphase confirms as a hit");
    }
    CHECK(candidates.size() >= true_hits.size(),
          "candidate set must be a superset of the confirmed narrowphase hits");
}

void testBroadphaseKnownQuery() {
    Nova::Math::SpatialClusterIndex index(1.0f, 4);
    std::vector<Nova::Math::ClusterAABB> boxes;

    // id 0 is the intended target; ids 1..3 are decoys in distant cells.
    boxes.push_back(makeBox(glm::vec3(0.5f, 0.5f, 0.5f), 0.25f));
    boxes.push_back(makeBox(glm::vec3(20.5f, 20.5f, 20.5f), 0.25f));
    boxes.push_back(makeBox(glm::vec3(-30.5f, 4.5f, 7.5f), 0.25f));
    boxes.push_back(makeBox(glm::vec3(11.5f, -18.5f, -6.5f), 0.25f));
    for (uint32_t i = 0; i < boxes.size(); ++i) {
        index.insert(i, boxes[i]);
    }
    CHECK(index.getItemCount() == boxes.size(), "index item count must match insert count");
    CHECK(index.getClusterCount() > 0, "index must allocate at least one cluster");

    Nova::Math::Ray3D ray(glm::vec3(0.5f, 0.5f, 10.0f), glm::vec3(0.0f, 0.0f, -1.0f));

    // Analytic narrowphase truth: front face of the target sits at z = 0.75.
    float t_min = 0.0f;
    float t_max = 0.0f;
    CHECK(boxes[0].intersectRay(ray, t_min, t_max), "target AABB must be hit by the query ray");
    CHECK_NEAR(t_min, 9.25, 1e-4, "analytic entry distance to the target AABB");

    testBroadphaseContainsNarrowphaseHits(boxes, index, ray, "known query must produce a narrowphase hit");

    uint32_t tests_performed = 0;
    Nova::Math::Ray3D far_ray(glm::vec3(500.0f, 500.0f, 500.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    std::vector<uint32_t> far_candidates = index.queryRay(far_ray, tests_performed);
    CHECK(!std::binary_search(far_candidates.begin(), far_candidates.end(), 0u),
          "grossly separated ray must not return the target AABB as a candidate");
}

// ---------------------------------------------------------------------------
// Halton low-discrepancy sequence.
// ---------------------------------------------------------------------------
void testHaltonSequence() {
    // The generator offsets by one internally, so halton(0, b) is the first
    // element of the canonical Halton sequence for base b.
    const double expect_base2[6] = {1.0 / 2, 1.0 / 4, 3.0 / 4, 1.0 / 8, 5.0 / 8, 3.0 / 8};
    const double expect_base3[6] = {1.0 / 3, 2.0 / 3, 1.0 / 9, 4.0 / 9, 7.0 / 9, 2.0 / 9};

    for (uint32_t i = 0; i < 6; ++i) {
        CHECK_NEAR(Nova::Math::InputRayFilter::halton(i, 2), expect_base2[i], 1e-6,
                   "Halton base-2 value mismatch");
        CHECK_NEAR(Nova::Math::InputRayFilter::halton(i, 3), expect_base3[i], 1e-6,
                   "Halton base-3 value mismatch");
    }

    bool in_range = true;
    for (uint32_t i = 0; i < 100000; ++i) {
        const float h2 = Nova::Math::InputRayFilter::halton(i, 2);
        const float h3 = Nova::Math::InputRayFilter::halton(i, 3);
        if (!(h2 >= 0.0f && h2 < 1.0f) || !(h3 >= 0.0f && h3 < 1.0f)) {
            in_range = false;
            break;
        }
    }
    CHECK(in_range, "every Halton output must lie in [0, 1)");
}

// ---------------------------------------------------------------------------
// Screen-ray filtering: identity inv-view-projection makes the expected ray
// analytic, so both the dithered and undithered paths can be pinned down.
// ---------------------------------------------------------------------------
// Max NDC displacement per axis: (dither_amplitude / 2) / (screen_extent / 2).
constexpr float kDitherNdcBoundX = 0.25f / 800.0f;
constexpr float kDitherNdcBoundY = 0.25f / 500.0f;

void testRayFilterDither() {
    Nova::Math::InputRayFilter filter;
    Nova::Math::EnginePhysicsConfig config;
    config.dither_enabled = false;
    config.dither_amplitude = 0.5f;

    const glm::vec2 base_pos(800.0f, 500.0f);
    const glm::vec2 screen_size(1600.0f, 1000.0f);
    const glm::mat4 inv_vp(1.0f);

    const Nova::Math::Ray3D undithered = filter.filterScreenRay(base_pos, screen_size, inv_vp, config, 0);
    CHECK_NEAR(undithered.origin.x, 0.0, 1e-6, "screen centre unprojects to NDC x = 0");
    CHECK_NEAR(undithered.origin.y, 0.0, 1e-6, "screen centre unprojects to NDC y = 0");
    CHECK_NEAR(undithered.direction.z, 1.0, 1e-6, "identity inv-VP yields a +Z view direction");
    CHECK_NEAR(glm::length(undithered.direction), 1.0, 1e-5, "ray direction must be unit length");

    config.dither_enabled = true;
    size_t distinct = 0;
    float previous = undithered.origin.x;
    for (uint32_t frame = 0; frame < 16; ++frame) {
        const Nova::Math::Ray3D r = filter.filterScreenRay(base_pos, screen_size, inv_vp, config, frame);
        CHECK(std::fabs(r.origin.x) <= kDitherNdcBoundX, "dither X offset must stay within the sub-pixel bound");
        CHECK(std::fabs(r.origin.y) <= kDitherNdcBoundY, "dither Y offset must stay within the sub-pixel bound");
        CHECK_NEAR(r.direction.z, 1.0, 1e-6, "dither must not perturb the ray direction");
        if (frame == 0 || r.origin.x != previous) {
            ++distinct;
        }
        previous = r.origin.x;
    }
    CHECK(distinct > 1, "enabling dither must actually perturb the sub-pixel sample position");
}

// ---------------------------------------------------------------------------
// Benchmarks (unchanged output, now with correctness assertions layered on).
// ---------------------------------------------------------------------------
void benchmarkSpatialIndex() {
    Nova::Math::SpatialClusterIndex cluster_index(1.0f, 4);
    const int num_aabbs = 100000;

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist_pos(-50.0f, 50.0f);
    std::uniform_real_distribution<float> dist_size(0.2f, 2.0f);

    std::vector<Nova::Math::ClusterAABB> boxes;
    boxes.reserve(num_aabbs);
    for (int i = 0; i < num_aabbs; ++i) {
        const glm::vec3 center(dist_pos(rng), dist_pos(rng), dist_pos(rng));
        const glm::vec3 half(dist_size(rng) * 0.5f);
        boxes.push_back(Nova::Math::ClusterAABB{center - half, center + half});
    }

    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_aabbs; ++i) {
        cluster_index.insert(static_cast<uint32_t>(i), boxes[i]);
    }
    auto end = std::chrono::high_resolution_clock::now();
    const double insert_sec = std::chrono::duration<double>(end - start).count();
    std::cout << " [SEAM 1 RESULT] Inserted " << num_aabbs << " spatial AABBs in "
              << insert_sec * 1000.0 << " ms (" << static_cast<double>(num_aabbs) / insert_sec << " AABBs/sec)" << std::endl;

    CHECK(cluster_index.getItemCount() == static_cast<size_t>(num_aabbs),
          "every inserted AABB must be accounted for");

    // Exhaustive no-false-negative sweep against the full 100k population.
    Nova::Math::Ray3D probe(glm::vec3(0.0f, 0.0f, 100.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    testBroadphaseContainsNarrowphaseHits(boxes, cluster_index, probe,
                                          "dense population must yield at least one narrowphase hit");

    const int num_rays = 500;
    const glm::vec3 ray_orig(0.0f, 0.0f, 100.0f);
    const glm::vec3 ray_dir(0.0f, 0.0f, -1.0f);

    start = std::chrono::high_resolution_clock::now();
    size_t total_candidates = 0;
    uint32_t tests_performed = 0;
    for (int i = 0; i < num_rays; ++i) {
        const glm::vec3 jittered_dir = glm::normalize(ray_dir + glm::vec3(dist_size(rng) * 0.01f, dist_size(rng) * 0.01f, 0.0f));
        Nova::Math::Ray3D ray{ray_orig, jittered_dir};
        total_candidates += cluster_index.queryRay(ray, tests_performed).size();
    }
    end = std::chrono::high_resolution_clock::now();
    const double ray_sec = std::chrono::duration<double>(end - start).count();
    std::cout << " [SEAM 1 RESULT] Executed " << num_rays << " broadphase raycast queries in "
              << ray_sec * 1000.0 << " ms (" << (ray_sec * 1e6) / num_rays << " us/ray, avg candidates: "
              << static_cast<double>(total_candidates) / num_rays << ")" << std::endl;

    CHECK(total_candidates > 0, "500 rays through a dense 100k population must find candidates");
    CHECK(tests_performed == cluster_index.getClusterCount(),
          "broadphase must test every cluster exactly once per query");
}

void benchmarkRayFilter() {
    Nova::Math::InputRayFilter ray_filter;
    Nova::Math::EnginePhysicsConfig config;
    config.dither_enabled = true;
    config.dither_amplitude = 0.5f;

    const int num_samples = 1000000;
    const glm::vec2 base_pos(800.0f, 500.0f);
    const glm::vec2 screen_size(1600.0f, 1000.0f);
    const glm::mat4 inv_vp(1.0f);

    const auto start = std::chrono::high_resolution_clock::now();
    double ray_accum = 0.0;
    float max_abs_origin_x = 0.0f;
    for (uint32_t i = 0; i < static_cast<uint32_t>(num_samples); ++i) {
        const Nova::Math::Ray3D r = ray_filter.filterScreenRay(base_pos, screen_size, inv_vp, config, i);
        ray_accum += static_cast<double>(r.origin.x) + static_cast<double>(r.direction.z);
        max_abs_origin_x = std::max(max_abs_origin_x, std::fabs(r.origin.x));
    }
    const auto end = std::chrono::high_resolution_clock::now();
    const double filter_sec = std::chrono::duration<double>(end - start).count();
    std::cout << " [SEAM 1 RESULT] Evaluated " << num_samples << " sub-pixel Halton temporal dithers in "
              << filter_sec * 1000.0 << " ms (" << static_cast<double>(num_samples) / filter_sec << " samples/sec)" << std::endl;

    // direction.z is exactly 1 for every sample, origin.x is a bounded sub-pixel
    // offset, so the mean accumulated term must converge on 1.0.
    CHECK_NEAR(ray_accum / num_samples, 1.0, 1e-3, "mean filtered ray term must converge on 1.0");
    CHECK(max_abs_origin_x <= kDitherNdcBoundX,
          "no dithered sample may exceed the sub-pixel NDC bound across the full sweep");
}

} // namespace

int main() {
    std::cout << "\n==========================================================================" << std::endl;
    std::cout << " [SEAM 1 BENCHMARK]: Mathematical Substrate & Raycast Engine Limits" << std::endl;
    std::cout << "==========================================================================" << std::endl;

    testRayAabbHitMiss();
    testRayAabbGraze();
    testBroadphaseKnownQuery();
    testHaltonSequence();
    testRayFilterDither();

    benchmarkSpatialIndex();
    benchmarkRayFilter();

    if (g_failures == 0) {
        std::cout << " [SEAM 1 STATUS] Math & Raycast Substrate PASSED with zero failures." << std::endl;
        return 0;
    }
    std::cout << " [SEAM 1 STATUS] Math & Raycast Substrate FAILED with " << g_failures << " failure(s)." << std::endl;
    return 1;
}
