#include <iostream>       // cout, cerr
#include <vector>         // std::vector
#include <string>         // std::string
#include <filesystem>     // C++17 paths
#include <stdexcept>      // Exceptions
#include <algorithm>      // std::transform
#include <cctype>         // ::tolower
#include <thread>         // std::thread
#include <mutex>          // std::mutex
#include <atomic>         // std::atomic
#include <sstream>        // std::stringstream
#include <queue>          // std::priority_queue
#include <cmath>          // std::ceil

#ifdef __APPLE__
#include <sys/sysctl.h>   // for sysctlbyname (macOS specific)
#include <mach/mach.h>    // for memory stats on macOS
#endif

#include <libheif/heif.h> // HEIF decoding
#include <jpeglib.h>      // JPEG encoding
#include <cstdio>         // fopen, fclose
#include <csetjmp>        // libjpeg error handling

namespace fs = std::filesystem; // Alias for filesystem

// Changes file extension
fs::path change_extension(const fs::path& input_path, const std::string& new_extension) {
    // Add '.' if missing
    std::string final_extension = new_extension;
    if (!final_extension.empty() && final_extension[0] != '.') {
        final_extension = "." + final_extension;
    }
    return input_path.parent_path() / (input_path.stem().string() + final_extension);
}

// Custom error handler for libjpeg
struct JpegErrorManager {
    jpeg_error_mgr pub;  // Standard error manager
    jmp_buf setjmp_buffer; // For longjmp on error
};

void jpeg_error_exit(j_common_ptr cinfo) {
    // Retrieve the custom error manager
    JpegErrorManager* err = reinterpret_cast<JpegErrorManager*>(cinfo->err);
    // Print error message
    (*cinfo->err->output_message)(cinfo);
    // Jump back to the setjmp point
    longjmp(err->setjmp_buffer, 1);
}

// Extract metadata from HEIC file
struct MetadataBlock {
    std::string type;
    std::vector<uint8_t> data;
};

std::vector<MetadataBlock> extract_metadata(heif_image_handle* handle) {
    std::vector<MetadataBlock> metadata_blocks;

    int num_metadata_blocks = heif_image_handle_get_number_of_metadata_blocks(handle, nullptr);
    if (num_metadata_blocks > 0) {
        std::vector<heif_item_id> metadata_ids(num_metadata_blocks);
        heif_image_handle_get_list_of_metadata_block_IDs(handle, nullptr, metadata_ids.data(), num_metadata_blocks);

        for (int i = 0; i < num_metadata_blocks; i++) {
            heif_item_id metadata_id = metadata_ids[i];
            const char* metadata_type = heif_image_handle_get_metadata_type(handle, metadata_id);
            if (!metadata_type) continue;

            size_t metadata_size = heif_image_handle_get_metadata_size(handle, metadata_id);
            if (metadata_size == 0) continue;

            std::vector<uint8_t> metadata_data(metadata_size);
            heif_error err = heif_image_handle_get_metadata(handle, metadata_id, metadata_data.data());
            if (err.code != heif_error_Ok) continue;

            metadata_blocks.push_back({metadata_type, metadata_data});
        }
    }

    return metadata_blocks;
}

// Preserve metadata in JPEG
void preserve_metadata(jpeg_compress_struct& cinfo, const std::vector<MetadataBlock>& metadata_blocks) {
    for (const auto& block : metadata_blocks) {
        if (block.type == "Exif") {
            std::vector<uint8_t> exif_data(6 + block.data.size());
            memcpy(exif_data.data(), "Exif\0\0", 6);
            memcpy(exif_data.data() + 6, block.data.data(), block.data.size());
            jpeg_write_marker(&cinfo, JPEG_APP0 + 1, exif_data.data(), exif_data.size());
        } else if (block.type == "XMP") {
            const char* xmp_ns = "http://ns.adobe.com/xap/1.0/";
            size_t ns_len = strlen(xmp_ns) + 1;
            std::vector<uint8_t> xmp_data(ns_len + block.data.size());
            memcpy(xmp_data.data(), xmp_ns, ns_len);
            memcpy(xmp_data.data() + ns_len, block.data.data(), block.data.size());
            jpeg_write_marker(&cinfo, JPEG_APP0 + 1, xmp_data.data(), xmp_data.size());
        } else if (block.type == "IPTC") {
            jpeg_write_marker(&cinfo, JPEG_APP0 + 13, block.data.data(), block.data.size());
        }
    }
}

// Thread-safe console output
std::mutex console_mutex;

void thread_safe_print(const std::string& message) {
    std::lock_guard<std::mutex> lock(console_mutex);
    std::cout << message << std::endl;
}

// Add these RAII-style wrappers for safer resource management
class HeifContextGuard {
private:
    heif_context* ctx;
public:
    HeifContextGuard() : ctx(heif_context_alloc()) {}
    ~HeifContextGuard() { if (ctx) heif_context_free(ctx); }
    heif_context* get() { return ctx; }
    operator bool() const { return ctx != nullptr; }
    // Prevent copying
    HeifContextGuard(const HeifContextGuard&) = delete;
    HeifContextGuard& operator=(const HeifContextGuard&) = delete;
};

class HeifImageHandleGuard {
private:
    heif_image_handle* handle;
public:
    HeifImageHandleGuard() : handle(nullptr) {}
    explicit HeifImageHandleGuard(heif_image_handle* h) : handle(h) {}
    ~HeifImageHandleGuard() { if (handle) heif_image_handle_release(handle); }
    heif_image_handle* get() { return handle; }
    operator bool() const { return handle != nullptr; }
    void reset(heif_image_handle* h) { 
        if (handle) heif_image_handle_release(handle);
        handle = h;
    }
    // Prevent copying
    HeifImageHandleGuard(const HeifImageHandleGuard&) = delete;
    HeifImageHandleGuard& operator=(const HeifImageHandleGuard&) = delete;
};

class HeifImageGuard {
private:
    heif_image* image;
public:
    HeifImageGuard() : image(nullptr) {}
    explicit HeifImageGuard(heif_image* img) : image(img) {}
    ~HeifImageGuard() { if (image) heif_image_release(image); }
    heif_image* get() { return image; }
    operator bool() const { return image != nullptr; }
    void reset(heif_image* img) {
        if (image) heif_image_release(image);
        image = img;
    }
    // Prevent copying
    HeifImageGuard(const HeifImageGuard&) = delete;
    HeifImageGuard& operator=(const HeifImageGuard&) = delete;
};

class FileGuard {
private:
    FILE* file;
public:
    explicit FileGuard(FILE* f) : file(f) {}
    ~FileGuard() { if (file) fclose(file); }
    FILE* get() { return file; }
    operator bool() const { return file != nullptr; }
    // Prevent copying
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
};

// Structure to hold image processing information with memory requirements
struct ImageJob {
    fs::path input_path;
    fs::path output_path;
    size_t estimated_memory_mb;
    
    // For sorting in priority queue (process smaller images first)
    bool operator<(const ImageJob& other) const {
        return estimated_memory_mb > other.estimated_memory_mb;
    }
};

// Get available system memory in MB
size_t get_available_memory_mb() {
    size_t available_memory = 0;
    
#ifdef __APPLE__
    // macOS implementation
    mach_port_t host_port = mach_host_self();
    mach_msg_type_number_t host_size = sizeof(vm_statistics64_data_t) / sizeof(integer_t);
    vm_size_t page_size;
    vm_statistics64_data_t vm_stats;
    
    host_page_size(host_port, &page_size);
    if (host_statistics64(host_port, HOST_VM_INFO64, 
                        (host_info64_t)&vm_stats, &host_size) == KERN_SUCCESS) {
        available_memory = (vm_stats.free_count + vm_stats.inactive_count) * page_size;
    }
#else
    // Fallback for non-macOS: assume 8GB system with 4GB available
    available_memory = 4ULL * 1024 * 1024 * 1024;
#endif

    // Convert to MB and return
    return available_memory / (1024 * 1024);
}

// Estimate memory needed for processing an image
size_t estimate_memory_requirement(const fs::path& image_path, heif_context* ctx = nullptr) {
    size_t total_memory_mb = 0;
    
    // Create a context if one wasn't provided
    HeifContextGuard local_ctx;
    if (!ctx) {
        ctx = local_ctx.get();
        if (!ctx) return 0;
        
        heif_error err = heif_context_read_from_file(ctx, image_path.c_str(), nullptr);
        if (err.code != heif_error_Ok) return 0;
    }
    
    // Get primary image handle
    heif_image_handle* handle = nullptr;
    heif_error err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok || !handle) {
        if (handle) heif_image_handle_release(handle);
        return 0;
    }
    
    // Get dimensions
    int width = heif_image_handle_get_width(handle);
    int height = heif_image_handle_get_height(handle);
    
    // Calculate memory requirements
    // 1. RGB decoded image: width * height * 3 bytes
    size_t rgb_memory = width * height * 3;
    
    // 2. JPEG compression buffer (conservative estimate)
    size_t jpeg_memory = width * height * 4; // Usually smaller, but allocate extra space
    
    // 3. Metadata and additional overhead (estimate: 10MB)
    size_t overhead_memory = 10 * 1024 * 1024;
    
    // Convert to MB with some safety margin (1.5x)
    total_memory_mb = static_cast<size_t>(
        std::ceil((rgb_memory + jpeg_memory + overhead_memory) * 1.5 / (1024 * 1024))
    );
    
    // Clean up if locally created
    if (handle) heif_image_handle_release(handle);
    
    return total_memory_mb;
}

// Helper function to get dimensions of a HEIF image
bool get_heif_dimensions(const fs::path& image_path, int& width, int& height) {
    HeifContextGuard ctx;
    if (!ctx) return false;
    
    heif_error err = heif_context_read_from_file(ctx.get(), image_path.c_str(), nullptr);
    if (err.code != heif_error_Ok) return false;
    
    HeifImageHandleGuard handle;
    heif_image_handle* temp_handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx.get(), &temp_handle);
    handle.reset(temp_handle);
    
    if (err.code != heif_error_Ok || !handle) return false;
    
    width = heif_image_handle_get_width(handle.get());
    height = heif_image_handle_get_height(handle.get());
    
    return true;
}

// Converts HEIF file to JPEG with dimension checks
bool convert_heif_to_jpeg(const fs::path& heif_path, const fs::path& jpeg_path, int quality, 
                          int max_width = 0, int max_height = 0, size_t max_memory_mb = 0) {
    std::stringstream log;
    log << "Converting '" << heif_path << "' to '" << jpeg_path << "'...";
    thread_safe_print(log.str());
    
    // Check image dimensions first if max dimensions specified
    if (max_width > 0 || max_height > 0) {
        int width = 0, height = 0;
        if (get_heif_dimensions(heif_path, width, height)) {
            if ((max_width > 0 && width > max_width) || (max_height > 0 && height > max_height)) {
                thread_safe_print("Error: Image dimensions (" + std::to_string(width) + "x" + 
                                 std::to_string(height) + ") exceed maximum allowed (" + 
                                 std::to_string(max_width) + "x" + std::to_string(max_height) + ")");
                return false;
            }
        }
    }
    
    // Check memory requirement if max memory specified
    if (max_memory_mb > 0) {
        size_t estimated_mem = estimate_memory_requirement(heif_path);
        if (estimated_mem > max_memory_mb) {
            thread_safe_print("Error: Estimated memory requirement (" + std::to_string(estimated_mem) + 
                             "MB) exceeds maximum allowed (" + std::to_string(max_memory_mb) + "MB)");
            return false;
        }
    }
    
    // === HEIF Decoding with RAII ===
    HeifContextGuard ctx;
    if (!ctx) {
        thread_safe_print("Error: Failed to allocate libheif context.");
        return false;
    }

    heif_error err = heif_context_read_from_file(ctx.get(), heif_path.c_str(), nullptr);
    if (err.code != heif_error_Ok) {
        thread_safe_print("Error: Failed to read HEIF file '" + heif_path.string() + "': " + err.message);
        return false;
    }

    // Get primary image handle
    HeifImageHandleGuard handle;
    heif_image_handle* temp_handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx.get(), &temp_handle);
    handle.reset(temp_handle);
    
    if (err.code != heif_error_Ok || !handle) {
        thread_safe_print("Error: Failed to get primary image handle from '" + heif_path.string() + "': " + 
                         (err.code ? err.message : "No primary image"));
        return false;
    }

    // Extract metadata
    std::vector<MetadataBlock> metadata_blocks = extract_metadata(handle.get());

    // Decode image to RGB
    HeifImageGuard img;
    heif_image* temp_img = nullptr;
    err = heif_decode_image(handle.get(), &temp_img, heif_colorspace_RGB, heif_chroma_interleaved_RGB, nullptr);
    img.reset(temp_img);
    
    if (err.code != heif_error_Ok || !img) {
        thread_safe_print("Error: Failed to decode HEIF image '" + heif_path.string() + "': " + (err.code ? err.message : "Decoding failed"));
        return false;
    }

    // Get image dimensions
    int width = heif_image_get_width(img.get(), heif_channel_interleaved);
    int height = heif_image_get_height(img.get(), heif_channel_interleaved);
    int stride = 0; // Row stride (bytes)
    const uint8_t* planar_data = heif_image_get_plane_readonly(img.get(), heif_channel_interleaved, &stride);

    if (!planar_data) {
         thread_safe_print("Error: Failed to get pixel data from decoded HEIF image '" + heif_path.string() + "'");
         return false;
    }

    // Create output directory if it doesn't exist
    fs::path output_dir = jpeg_path.parent_path();
    if (!output_dir.empty() && !fs::exists(output_dir)) {
        std::error_code ec;
        if (!fs::create_directories(output_dir, ec)) {
            thread_safe_print("Error: Failed to create output directory '" + output_dir.string() + "': " + ec.message());
            return false;
        }
        thread_safe_print("Created output directory: " + output_dir.string());
    }

    // === JPEG Encoding ===
    struct jpeg_compress_struct cinfo;
    struct JpegErrorManager jerr; // Custom error manager

    // Open output JPEG file (binary write)
    FILE* outfile_ptr = fopen(jpeg_path.c_str(), "wb");
    if (!outfile_ptr) {
        thread_safe_print("Error: Cannot open output file '" + jpeg_path.string() + "' for writing.");
        return false;
    }
    
    FileGuard outfile(outfile_ptr);

    // Setup custom error handling
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;
    if (setjmp(jerr.setjmp_buffer)) {
        // Handle error - resources are automatically cleaned up by RAII guards
        thread_safe_print("Error: libjpeg encountered an error during compression.");
        jpeg_destroy_compress(&cinfo);
        return false;
    }

    // Create compression object
    jpeg_create_compress(&cinfo);
    
    // Make sure we clean up even if there's an exception or early return
    struct CompressGuard {
        jpeg_compress_struct* cinfo;
        CompressGuard(jpeg_compress_struct* c) : cinfo(c) {}
        ~CompressGuard() { jpeg_destroy_compress(cinfo); }
    } compress_guard(&cinfo);
    
    // Set output destination
    jpeg_stdio_dest(&cinfo, outfile.get());

    // Set JPEG image parameters
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;     // 3 components for RGB
    cinfo.in_color_space = JCS_RGB; // Input is RGB

    // Set compression parameters
    jpeg_set_defaults(&cinfo);            // Default JPEG params
    jpeg_set_quality(&cinfo, quality, TRUE); // Set quality [1-100]

    // Start compression process
    jpeg_start_compress(&cinfo, TRUE);

    // Write metadata blocks to JPEG
    preserve_metadata(cinfo, metadata_blocks);

    // Write scanlines
    JSAMPROW row_pointer[1]; // Pointer to row data
    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = const_cast<JSAMPROW>(&planar_data[cinfo.next_scanline * stride]);
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    // Finish compression
    jpeg_finish_compress(&cinfo);

    thread_safe_print("Successfully saved '" + jpeg_path.string() + "'");
    return true;
}

// Worker function for processing a single file with memory and dimension limits
void process_file(const fs::path& input_path, const fs::path& output_path, int quality, 
                  bool force_overwrite, int max_width, int max_height, size_t max_memory_mb,
                  std::atomic<int>& success_count, std::atomic<int>& fail_count, std::atomic<int>& skip_count) {
    // Check file existence and type
    if (!fs::exists(input_path)) {
        thread_safe_print("Error: Input file not found: " + input_path.string());
        fail_count++;
        return;
    }
    if (!fs::is_regular_file(input_path)) {
        thread_safe_print("Error: Input is not a regular file: " + input_path.string());
        fail_count++;
        return;
    }

    // Check file extension (.heic/.heif)
    std::string ext = input_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".heic" && ext != ".heif") {
        thread_safe_print("Warning: Skipping non-HEIC/HEIF file: " + input_path.string());
        skip_count++;
        return;
    }

    // Check if output exists
    if (fs::exists(output_path) && !force_overwrite) {
        thread_safe_print("Warning: Output file " + output_path.string() + " already exists. Skipping conversion for " + input_path.string());
        skip_count++;
        return;
    }

    // Convert the file with dimension and memory limits
    if (convert_heif_to_jpeg(input_path, output_path, quality, max_width, max_height, max_memory_mb)) {
        success_count++;
    } else {
        fail_count++;
    }
}

// Batch processor for memory-efficient processing
class BatchProcessor {
private:
    std::priority_queue<ImageJob> job_queue;
    std::mutex queue_mutex;
    std::atomic<bool> processing_complete{false};
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};
    std::atomic<int> skip_count{0};
    int quality;
    bool force_overwrite;
    int max_width;
    int max_height;
    size_t memory_per_thread_mb;
    unsigned int thread_count;
    
public:
    BatchProcessor(int quality, bool force_overwrite, int max_width, int max_height, 
                   size_t memory_budget_mb, unsigned int thread_count)
        : quality(quality), force_overwrite(force_overwrite), max_width(max_width), 
          max_height(max_height), thread_count(thread_count) {
        // Divide memory budget by thread count, but ensure at least 100MB per thread
        memory_per_thread_mb = std::max(100UL, memory_budget_mb / thread_count);
    }
    
    void add_job(const fs::path& input_path, const fs::path& output_path) {
        size_t mem_req = estimate_memory_requirement(input_path);
        
        ImageJob job{input_path, output_path, mem_req};
        
        std::lock_guard<std::mutex> lock(queue_mutex);
        job_queue.push(job);
    }
    
    void process_all() {
        std::vector<std::thread> thread_pool;
        
        // Start worker threads
        for (unsigned int i = 0; i < thread_count; i++) {
            thread_pool.emplace_back(&BatchProcessor::worker_thread, this);
        }
        
        // Wait for all threads to complete
        for (auto& thread : thread_pool) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
    
    void worker_thread() {
        while (true) {
            // Get next job from queue
            ImageJob current_job;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (job_queue.empty()) {
                    return; // No more work
                }
                
                current_job = job_queue.top();
                job_queue.pop();
            }
            
            // Check if job exceeds memory limit for this thread
            if (current_job.estimated_memory_mb > memory_per_thread_mb) {
                thread_safe_print("Warning: Image " + current_job.input_path.string() + 
                                 " requires " + std::to_string(current_job.estimated_memory_mb) + 
                                 "MB which exceeds per-thread limit of " + 
                                 std::to_string(memory_per_thread_mb) + "MB");
                
                // Try processing with reduced memory constraint anyway
                process_file(current_job.input_path, current_job.output_path, quality, 
                            force_overwrite, max_width, max_height, memory_per_thread_mb,
                            success_count, fail_count, skip_count);
            } else {
                // Process normally
                process_file(current_job.input_path, current_job.output_path, quality, 
                            force_overwrite, max_width, max_height, memory_per_thread_mb,
                            success_count, fail_count, skip_count);
            }
        }
    }
    
    // Get results
    int get_success_count() const { return success_count.load(); }
    int get_fail_count() const { return fail_count.load(); }
    int get_skip_count() const { return skip_count.load(); }
};

// Function to get the number of performance cores on macOS
unsigned int get_performance_core_count() {
    unsigned int performance_cores = 0;
    
#ifdef __APPLE__
    // Try to get the number of performance cores (Apple-specific)
    int perf_cores = 0;
    size_t size = sizeof(perf_cores);
    
    // Try to read performance core count
    if (sysctlbyname("hw.perflevel0.physicalcpu", &perf_cores, &size, NULL, 0) == 0 && perf_cores > 0) {
        performance_cores = perf_cores;
        thread_safe_print("Detected " + std::to_string(performance_cores) + " performance cores");
    } else {
        // Fallback: try to get total physical CPU count
        if (sysctlbyname("hw.physicalcpu", &perf_cores, &size, NULL, 0) == 0 && perf_cores > 0) {
            // On non-hybrid architectures, use half of physical cores as an approximation
            performance_cores = (perf_cores + 1) / 2;
            thread_safe_print("Using " + std::to_string(performance_cores) + " threads (half of " + 
                             std::to_string(perf_cores) + " physical cores)");
        }
    }
#endif

    // If we couldn't determine the performance core count, use a reasonable default
    if (performance_cores == 0) {
        // Use half of available logical cores or at least 2
        performance_cores = std::max(2u, std::thread::hardware_concurrency() / 2);
        thread_safe_print("Using default of " + std::to_string(performance_cores) + " threads");
    }
    
    return performance_cores;
}

// Program entry point
int main(int argc, char *argv[]) {
    int quality = 95;                 // Default JPEG quality (1-100)
    bool force_overwrite = false;     // Default: do not overwrite existing files
    std::vector<std::string> input_filenames; // Input filenames
    fs::path output_directory;        // Optional output directory
    
    // New parameters for memory and dimension limits
    int max_width = 0;                // Default: no limit (0 = unlimited)
    int max_height = 0;               // Default: no limit (0 = unlimited)
    size_t memory_budget_mb = 0;      // Default: no limit (0 = unlimited)
    bool auto_memory_budget = true;   // Default: use 75% of available memory
    bool show_help = false;           // Flag to show help message
    
    // Get performance core count automatically
    unsigned int max_threads = get_performance_core_count();

    // Argument parsing loop
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        
        // Check for help flag first
        if (arg == "-h" || arg == "--help") {
            show_help = true;
            break;
        }
        // Quality parameter
        else if (arg == "-q" || arg == "--quality" || arg == "-quality") {
            if (i + 1 < argc) {
                try {
                    int parsed_quality = std::stoi(argv[i + 1]);
                    if (parsed_quality >= 1 && parsed_quality <= 100) {
                        quality = parsed_quality;
                        i++;
                        continue; // Skip flag and value
                    } else {
                        std::cerr << "Error: Quality value must be between 1 and 100. Found: " << argv[i + 1] << std::endl;
                        return 1;
                    }
                } catch (const std::invalid_argument& e) {
                    std::cerr << "Error: Invalid number format for quality: " << argv[i + 1] << std::endl;
                    return 1;
                } catch (const std::out_of_range& e) {
                    std::cerr << "Error: Quality value out of range: " << argv[i + 1] << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: Missing value after quality flag." << std::endl;
                return 1;
            }
        } 
        // Force overwrite parameter
        else if (arg == "-f" || arg == "--force" || arg == "-force") {
            force_overwrite = true;
        } 
        // Output directory parameter
        else if (arg == "-o" || arg == "--outdir" || arg == "-outdir") {
            if (i + 1 < argc) {
                output_directory = argv[i + 1];
                i++;
                continue;
            } else {
                std::cerr << "Error: Missing path after output directory flag." << std::endl;
                return 1;
            }
        } 
        // Max width parameter
        else if (arg == "-w" || arg == "--maxwidth" || arg == "-maxwidth") {
            if (i + 1 < argc) {
                try {
                    max_width = std::stoi(argv[i + 1]);
                    if (max_width < 0) {
                        std::cerr << "Error: Max width must be positive. Found: " << argv[i + 1] << std::endl;
                        return 1;
                    }
                    i++;
                } catch (const std::exception& e) {
                    std::cerr << "Error: Invalid number format for max width: " << argv[i + 1] << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: Missing value after max width flag." << std::endl;
                return 1;
            }
        } 
        // Max height parameter
        else if (arg == "-ht" || arg == "--maxheight" || arg == "-maxheight") {
            if (i + 1 < argc) {
                try {
                    max_height = std::stoi(argv[i + 1]);
                    if (max_height < 0) {
                        std::cerr << "Error: Max height must be positive. Found: " << argv[i + 1] << std::endl;
                        return 1;
                    }
                    i++;
                } catch (const std::exception& e) {
                    std::cerr << "Error: Invalid number format for max height: " << argv[i + 1] << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: Missing value after max height flag." << std::endl;
                return 1;
            }
        } 
        // Memory budget parameter
        else if (arg == "-m" || arg == "--memory" || arg == "-memory") {
            if (i + 1 < argc) {
                try {
                    memory_budget_mb = std::stoul(argv[i + 1]);
                    if (memory_budget_mb < 100) {
                        std::cerr << "Error: Memory budget must be at least 100MB. Found: " << argv[i + 1] << std::endl;
                        return 1;
                    }
                    auto_memory_budget = false;
                    i++;
                } catch (const std::exception& e) {
                    std::cerr << "Error: Invalid number format for memory budget: " << argv[i + 1] << std::endl;
                    return 1;
                }
            } else {
                std::cerr << "Error: Missing value after memory flag." << std::endl;
                return 1;
            }
        } else {
            // Treat as filename
            input_filenames.push_back(argv[i]);
        }
    }

    // Display help message
    if (show_help || input_filenames.empty()) {
        std::cout << "Usage: " << argv[0] << " [OPTIONS] <input_file.heic> [input_file2.heif] ..." << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  -q, --quality N:   Set JPEG quality (1-100, default: 95)" << std::endl;
        std::cout << "  -f, --force:       Overwrite existing output files" << std::endl;
        std::cout << "  -o, --outdir PATH: Set output directory for converted images" << std::endl;
        std::cout << "  -w, --maxwidth N:  Set maximum allowed image width (0 = unlimited)" << std::endl;
        std::cout << "  -ht, --maxheight N: Set maximum allowed image height (0 = unlimited)" << std::endl;
        std::cout << "  -m, --memory MB:   Set memory budget in MB (0 = auto)" << std::endl;
        std::cout << "  -h, --help:        Display this help message" << std::endl;
        std::cout << std::endl;
        std::cout << "Note: Wildcards like *.heic are expanded by your shell." << std::endl;
        return show_help ? 0 : 1;  // Return success if help was requested, error if no input files
    }

    // Create output directory if specified and doesn't exist
    if (!output_directory.empty() && !fs::exists(output_directory)) {
        std::error_code ec;
        if (!fs::create_directories(output_directory, ec)) {
            std::cerr << "Error: Failed to create output directory '" << output_directory << "': " << ec.message() << std::endl;
            return 1;
        }
        std::cout << "Created output directory: " << output_directory << std::endl;
    }

    // Calculate memory budget if automatic
    if (auto_memory_budget) {
        size_t available_mem = get_available_memory_mb();
        // Use 75% of available memory
        memory_budget_mb = available_mem * 3 / 4;
        std::cout << "Automatic memory budget: " << memory_budget_mb << "MB (75% of " 
                  << available_mem << "MB available)" << std::endl;
    } else {
        std::cout << "User-specified memory budget: " << memory_budget_mb << "MB" << std::endl;
    }
    
    if (max_width > 0 || max_height > 0) {
        std::cout << "Maximum image dimensions: " 
                  << (max_width > 0 ? std::to_string(max_width) : "unlimited") << " x " 
                  << (max_height > 0 ? std::to_string(max_height) : "unlimited") << std::endl;
    }

    // Create batch processor
    BatchProcessor processor(quality, force_overwrite, max_width, max_height, memory_budget_mb, max_threads);
    
    // Prepare all jobs
    for (const auto& input_filename : input_filenames) {
        fs::path input_path(input_filename);
        
        // Determine output path
        fs::path output_path;
        if (output_directory.empty()) {
            output_path = change_extension(input_path, ".jpg");
        } else {
            output_path = output_directory / change_extension(input_path.filename(), ".jpg");
        }
        
        processor.add_job(input_path, output_path);
    }
    
    // Process all images
    std::cout << "Starting batch processing with " << max_threads << " threads ..." << std::endl;
    processor.process_all();

    // === Summary ===
    std::cout << "----------------------------------------" << std::endl;
    std::cout << "Processing finished." << std::endl;
    std::cout << "  Successful conversions: " << processor.get_success_count() << std::endl;
    std::cout << "  Skipped (output exists): " << processor.get_skip_count() << std::endl;
    std::cout << "  Failed conversions:     " << processor.get_fail_count() << std::endl;
    std::cout << "  Worker threads used:    " << max_threads << std::endl;
    std::cout << "  Memory budget:          " << memory_budget_mb << "MB" << std::endl;

    // Return 1 on failure, 0 on success
    return (processor.get_fail_count() > 0) ? 1 : 0;
}