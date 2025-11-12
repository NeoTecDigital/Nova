#pragma once

#include "Nova.h"
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <mutex>
#include <functional>

/**
 * NovaMLManager - GPU-accelerated Machine Learning Manager
 * 
 * This class provides GPU-accelerated machine learning operations using the Nova Vulkan engine.
 * It handles CNN operations, action generation, and screen analysis on the GPU.
 */
class NovaMLManager {
public:
    struct MLConfig {
        // GPU settings
        uint32_t compute_queue_count = 2;
        uint32_t max_gpu_memory_mb = 2048;
        bool enable_gpu_fallback = true;
        
        // CNN settings
        uint32_t cnn_input_width = 224;
        uint32_t cnn_input_height = 224;
        uint32_t cnn_channels = 3;
        uint32_t cnn_layers = 5;
        
        // Performance settings
        uint32_t batch_size = 32;
        float learning_rate = 0.001f;
        uint32_t max_iterations = 1000;
    };

    struct ScreenAnalysisResult {
        std::vector<float> object_probabilities;
        std::vector<std::pair<int, int>> object_locations;  // (x, y) coordinates
        std::vector<std::string> object_labels;
        float confidence_score;
        uint64_t processing_time_ms;
    };

    struct ActionSuggestion {
        std::string action_type;
        std::map<std::string, std::string> parameters;
        float confidence;
        std::string reasoning;
    };

    NovaMLManager(const MLConfig& config);
    ~NovaMLManager();

    // Initialization and setup
    bool initialize();
    bool isInitialized() const;
    void shutdown();

    // GPU resource management
    bool checkGPUResources() const;
    double getGPUMemoryUsage() const;
    double getGPUUtilization() const;
    bool isGPUSafe() const;

    // CNN operations
    bool initializeCNN();
    ScreenAnalysisResult analyzeScreen(const std::vector<uint8_t>& screen_data);
    std::vector<float> extractFeatures(const std::vector<uint8_t>& input_data);
    bool updateCNNWeights(const std::vector<float>& gradients);

    // Action generation
    std::vector<ActionSuggestion> generateActions(const ScreenAnalysisResult& analysis);
    bool validateAction(const ActionSuggestion& action, uint32_t screen_width, uint32_t screen_height);
    float predictActionSuccess(const ActionSuggestion& action, const ScreenAnalysisResult& analysis);

    // Learning operations
    bool trainOnFeedback(const ActionSuggestion& action, bool was_successful, const ScreenAnalysisResult& context);
    bool saveModelState(const std::string& filename);
    bool loadModelState(const std::string& filename);

    // Performance monitoring
    struct PerformanceMetrics {
        double average_inference_time_ms;
        double average_training_time_ms;
        double gpu_memory_usage_percent;
        double gpu_utilization_percent;
        uint64_t total_operations;
        uint64_t successful_operations;
        float success_rate;
    };

    PerformanceMetrics getPerformanceMetrics() const;
    void resetPerformanceMetrics();

private:
    MLConfig config_;
    std::unique_ptr<Nova> nova_engine_;
    bool initialized_;
    
    // GPU compute resources
    VkCommandPool compute_command_pool_;
    VkCommandBuffer compute_command_buffer_;
    VkFence compute_fence_;
    
    // CNN resources
    VkBuffer cnn_input_buffer_;
    VkBuffer cnn_output_buffer_;
    VkBuffer cnn_weights_buffer_;
    VkDeviceMemory cnn_memory_;
    
    // Performance tracking
    mutable PerformanceMetrics metrics_;
    mutable std::mutex metrics_mutex_;
    
    // Internal methods
    bool createComputeResources();
    bool createCNNBuffers();
    bool compileComputeShaders();
    bool setupComputePipeline();
    
    // GPU operations
    bool executeGPUCompute(const std::vector<float>& input, std::vector<float>& output);
    bool uploadToGPU(const std::vector<float>& data, VkBuffer buffer);
    bool downloadFromGPU(VkBuffer buffer, std::vector<float>& data);
    
    // CNN specific operations
    bool runCNNInference(const std::vector<uint8_t>& input, std::vector<float>& output);
    bool runCNNTraining(const std::vector<float>& input, const std::vector<float>& target, std::vector<float>& gradients);
    
    // Utility methods
    std::vector<float> preprocessImage(const std::vector<uint8_t>& raw_image);
    std::vector<uint8_t> postprocessOutput(const std::vector<float>& cnn_output);
    bool validateGPUMemory(size_t required_bytes);
    
    // Error handling
    void logGPUError(const std::string& operation, VkResult result);
    bool handleGPUError(VkResult result, const std::string& operation);
}; 