#pragma once

#include "./quaternion_transform.h"
#include "./raycast.h"
#include <vector>
#include <unordered_map>
#include <memory>
#include <algorithm>

namespace NovaMath {

struct ClusterAABB {
    glm::vec3 min_pt{-1.0f};
    glm::vec3 max_pt{1.0f};

    bool intersectRay(const Ray3D& ray, float& out_t_min, float& out_t_max) const {
        float tmin = (min_pt.x - ray.origin.x) / (std::abs(ray.direction.x) > 1e-7f ? ray.direction.x : 1e-7f);
        float tmax = (max_pt.x - ray.origin.x) / (std::abs(ray.direction.x) > 1e-7f ? ray.direction.x : 1e-7f);
        if (tmin > tmax) std::swap(tmin, tmax);

        float tymin = (min_pt.y - ray.origin.y) / (std::abs(ray.direction.y) > 1e-7f ? ray.direction.y : 1e-7f);
        float tymax = (max_pt.y - ray.origin.y) / (std::abs(ray.direction.y) > 1e-7f ? ray.direction.y : 1e-7f);
        if (tymin > tymax) std::swap(tymin, tymax);

        if ((tmin > tymax) || (tymin > tmax)) return false;
        if (tymin > tmin) tmin = tymin;
        if (tymax < tmax) tmax = tymax;

        float tzmin = (min_pt.z - ray.origin.z) / (std::abs(ray.direction.z) > 1e-7f ? ray.direction.z : 1e-7f);
        float tzmax = (max_pt.z - ray.origin.z) / (std::abs(ray.direction.z) > 1e-7f ? ray.direction.z : 1e-7f);
        if (tzmin > tzmax) std::swap(tzmin, tzmax);

        if ((tmin > tzmax) || (tzmin > tmax)) return false;
        if (tzmin > tmin) tmin = tzmin;
        if (tzmax < tmax) tmax = tzmax;

        out_t_min = tmin;
        out_t_max = tmax;
        return tmax >= std::max(0.0f, tmin);
    }
};

/**
 * SpatialClusterIndex - Hierarchical (Precision * Depth) Spatial Indexing
 * 
 * Partitions 3D space into hierarchical cluster nodes to accelerate ray intersections
 * and broadphase spatial queries.
 */
class SpatialClusterIndex {
public:
    SpatialClusterIndex(float cell_size = 1.0f, int max_depth = 4)
        : base_cell_size_(cell_size), max_depth_(max_depth) {}

    void clear() {
        clusters_.clear();
        item_count_ = 0;
    }

    uint64_t computeKey(const glm::vec3& pos, int depth) const {
        float scale = base_cell_size_ / static_cast<float>(1 << std::clamp(depth, 0, 16));
        int64_t ix = static_cast<int64_t>(std::floor(pos.x / scale));
        int64_t iy = static_cast<int64_t>(std::floor(pos.y / scale));
        int64_t iz = static_cast<int64_t>(std::floor(pos.z / scale));

        // Spatial Morton / Interleaved Hash Key
        uint64_t key = (static_cast<uint64_t>(depth) & 0x0F) << 60;
        key |= (static_cast<uint64_t>(ix & 0xFFFFF) << 40);
        key |= (static_cast<uint64_t>(iy & 0xFFFFF) << 20);
        key |= (static_cast<uint64_t>(iz & 0xFFFFF));
        return key;
    }

    struct ClusterNode {
        uint64_t key = 0;
        int depth = 0;
        ClusterAABB bounds;
        std::vector<uint32_t> item_ids;
    };

    void insert(uint32_t item_id, const ClusterAABB& bounds, int depth = 0) {
        glm::vec3 center = (bounds.min_pt + bounds.max_pt) * 0.5f;
        uint64_t key = computeKey(center, depth);

        auto& cluster = clusters_[key];
        cluster.key = key;
        cluster.depth = depth;
        if (cluster.item_ids.empty()) {
            cluster.bounds = bounds;
        } else {
            cluster.bounds.min_pt = glm::min(cluster.bounds.min_pt, bounds.min_pt);
            cluster.bounds.max_pt = glm::max(cluster.bounds.max_pt, bounds.max_pt);
        }
        cluster.item_ids.push_back(item_id);
        item_count_++;
    }

    // Intersect ray against cluster bounding boxes (accelerated broadphase)
    std::vector<uint32_t> queryRay(const Ray3D& ray, uint32_t& out_tests_performed) const {
        std::vector<uint32_t> candidates;
        out_tests_performed = 0;

        for (const auto& pair : clusters_) {
            out_tests_performed++;
            float tmin, tmax;
            if (pair.second.bounds.intersectRay(ray, tmin, tmax)) {
                candidates.insert(candidates.end(), pair.second.item_ids.begin(), pair.second.item_ids.end());
            }
        }

        // Deduplicate
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        return candidates;
    }

    size_t getClusterCount() const { return clusters_.size(); }
    size_t getItemCount() const { return item_count_; }

private:
    float base_cell_size_ = 1.0f;
    int max_depth_ = 4;
    size_t item_count_ = 0;
    std::unordered_map<uint64_t, ClusterNode> clusters_;
};

} // namespace NovaMath
