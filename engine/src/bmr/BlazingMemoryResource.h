#pragma once 

#include <cassert>
#include <atomic>

#include <cuda_runtime_api.h>

#include <rmm/rmm_api.h>
#include <rmm/detail/memory_manager.hpp>
#include <rmm/mr/device/device_memory_resource.hpp>
#include <rmm/mr/device/cuda_memory_resource.hpp>
#include <rmm/mr/device/managed_memory_resource.hpp>
#include <rmm/mr/device/cnmem_managed_memory_resource.hpp>
#include <rmm/mr/device/cnmem_memory_resource.hpp>

#include "config/GPUManager.cuh"

#include <sys/sysinfo.h>
#include <sys/statvfs.h>


class BlazingMemoryResource {
public:
	virtual std::size_t get_from_driver_available_memory() = 0 ; // driver.get_available_memory()
	virtual std::size_t get_memory_limit() = 0 ; // memory_limite = total_memory * threshold

	virtual std::size_t get_memory_used() = 0 ; // atomic 
	virtual std::size_t get_total_memory() = 0 ; // total_memory
};

class internal_blazing_device_memory_resource : public rmm::mr::device_memory_resource { 
public:
    // TODO: use another constructor for memory in bytes

	internal_blazing_device_memory_resource(rmmOptions_t rmmValues, float custom_threshold = 0.75)
    {
		total_memory_size = ral::config::gpuMemorySize();
		used_memory = 0;

		rmmAllocationMode_t allocation_mode = static_cast<rmmAllocationMode_t>(rmmValues.allocation_mode);
		if (allocation_mode == (CudaManagedMemory | PoolAllocation) || allocation_mode == PoolAllocation) {
			if (total_memory_size <= rmmValues.initial_pool_size) {
				throw std::runtime_error("Cannot allocate this Pool memory size on the GPU.");
			} 

			if (allocation_mode == CudaManagedMemory | PoolAllocation) {
				memory_resource = std::make_unique<rmm::mr::cnmem_managed_memory_resource>();
			} else {
				memory_resource = std::make_unique<rmm::mr::cnmem_memory_resource>();
			}
		} 
		else if (allocation_mode == CudaManagedMemory) {
			memory_resource = std::make_unique<rmm::mr::managed_memory_resource>();
		}
		else { // allocation_mode == useCudaDefaultAllocator
			memory_resource = std::make_unique<rmm::mr::cuda_memory_resource>();
		}
        memory_limit = custom_threshold * total_memory_size;
	}

	virtual ~internal_blazing_device_memory_resource() = default;

	std::size_t get_memory_used() {
		return used_memory;
	}
    std::size_t get_from_driver_available_memory() {
	    return ral::config::gpuUsedMemory();
    }

	std::size_t get_total_memory() {
		return total_memory_size;
	}
	std::size_t get_memory_limit() {
        return memory_limit;
    }

	bool supports_streams() const noexcept override { return memory_resource->supports_streams(); }
	bool supports_get_mem_info() const noexcept override { return memory_resource->supports_get_mem_info(); }

private: 
	void* do_allocate(std::size_t bytes, cudaStream_t stream) override {
		if (bytes <= 0) { 
            return nullptr;
		}
		used_memory += bytes;
		return memory_resource->allocate(bytes, stream);
	}

	void do_deallocate(void* p, std::size_t bytes, cudaStream_t stream) override {
		if (nullptr == p || bytes == 0) return;
		if (used_memory < bytes) {
			std::cerr << "blazing_device_memory_resource: Deallocating more bytes than used right now, used_memory: " << used_memory << " less than " << bytes << " bytes." << std::endl;
			used_memory = 0;
		} else {
			used_memory -= bytes;
		}

		return memory_resource->deallocate(p, bytes, stream);
	}

	bool do_is_equal(device_memory_resource const& other) const noexcept override {
		return memory_resource->is_equal(other);
	}

	std::pair<size_t, size_t> do_get_mem_info(cudaStream_t stream) const override {
		return memory_resource->get_mem_info(stream);
	}

	std::size_t total_memory_size;
	std::size_t memory_limit;
	std::atomic<std::size_t> used_memory;
    std::unique_ptr<rmm::mr::device_memory_resource> memory_resource;
};

// forward declaration
typedef struct CUstream_st *cudaStream_t;

/** -------------------------------------------------------------------------*
 * @brief RMM blazing_device_memory_resource class maintains the memory manager context, including
 * the RMM event log, configuration options, and registered streams.
 * 
 * blazing_device_memory_resource is a singleton class, and should be accessed via getInstance(). 
 * A number of static convenience methods are provided that wrap getInstance(),
 * such as getLogger() and getOptions().
 * ------------------------------------------------------------------------**/
class blazing_device_memory_resource : public BlazingMemoryResource {
public:
    /** -----------------------------------------------------------------------*
     * @brief Get the blazing_device_memory_resource instance singleton object
     * 
     * @return blazing_device_memory_resource& the blazing_device_memory_resource singleton
     * ----------------------------------------------------------------------**/
    static blazing_device_memory_resource& getInstance(){
        // Myers' singleton. Thread safe and unique. Note: C++11 required.
        static blazing_device_memory_resource instance;
        return instance;
    }

	std::size_t get_memory_used() {
		// std::cout << "blazing_device_memory_resource: " << initialized_resource->get_memory_used() << std::endl; 
		return initialized_resource->get_memory_used();
	}

	std::size_t get_total_memory() {
		return initialized_resource->get_total_memory() ;
	}

    std::size_t get_from_driver_available_memory()  {
        return initialized_resource->get_from_driver_available_memory();
    }
	std::size_t get_memory_limit() {
		// JUST FOR DEBUGGING: return 6511224;
		return initialized_resource->get_memory_limit() ;
    }

  /** -----------------------------------------------------------------------*
   * @brief Initialize RMM options
   * 
   * Accepts an optional rmmOptions_t struct that describes the settings used
   * to initialize the memory manager. If no `options` is passed, default
   * options are used.
   * 
   * @param[in] options Optional options to set
   * ----------------------------------------------------------------------**/
    void initialize(const rmmOptions_t *new_options) {
        
        std::lock_guard<std::mutex> guard(manager_mutex);

        // repeat initialization is a no-op
        if (isInitialized()) return;

        if (nullptr != new_options) options = *new_options;

        initialized_resource.reset(new internal_blazing_device_memory_resource(options));
        
        rmm::mr::set_default_resource(initialized_resource.get());
        
        is_initialized = true;
    }

    /** -----------------------------------------------------------------------*
     * @brief Shut down the blazing_device_memory_resource (clears the context)
     * ----------------------------------------------------------------------**/
    void finalize(){
        std::lock_guard<std::mutex> guard(manager_mutex);

        // finalization before initialization is a no-op
        if (isInitialized()) {
            registered_streams.clear();
            initialized_resource.reset();
            is_initialized = false;
        }
    }

    /** -----------------------------------------------------------------------*
     * @brief Check whether the blazing_device_memory_resource has been initialized.
     * 
     * @return true if blazing_device_memory_resource has been initialized.
     * @return false if blazing_device_memory_resource has not been initialized.
     * ----------------------------------------------------------------------**/
    bool isInitialized() {
        return getInstance().is_initialized;
    }

    /** -----------------------------------------------------------------------*
     * @brief Get the Options object
     * 
     * @return rmmOptions_t the currently set RMM options
     * ----------------------------------------------------------------------**/
    static rmmOptions_t getOptions() { return getInstance().options; }

    /** -----------------------------------------------------------------------*
     * @brief Returns true when pool allocation is enabled
     * 
     * @return true if pool allocation is enabled
     * @return false if pool allocation is disabled
     * ----------------------------------------------------------------------**/
    static inline bool usePoolAllocator() {
        return getOptions().allocation_mode & PoolAllocation;
    }

    /** -----------------------------------------------------------------------*
     * @brief Returns true if CUDA Managed Memory allocation is enabled
     * 
     * @return true if CUDA Managed Memory allocation is enabled
     * @return false if CUDA Managed Memory allocation is disabled
     * ----------------------------------------------------------------------**/
    static inline bool useManagedMemory() {
        return getOptions().allocation_mode & CudaManagedMemory;
    }

    /** -----------------------------------------------------------------------*
     * @brief Returns true when CUDA default allocation is enabled
     *          * 
     * @return true if CUDA default allocation is enabled
     * @return false if CUDA default allocation is disabled
     * ----------------------------------------------------------------------**/
    inline bool useCudaDefaultAllocator() {
        return CudaDefaultAllocation == getOptions().allocation_mode;
    }

    /** -----------------------------------------------------------------------*
     * @brief Register a new stream into the device memory manager.
     * 
     * Also returns success if the stream is already registered.
     * 
     * @param stream The stream to register
     * @return rmmError_t RMM_SUCCESS if all goes well,
     *                    RMM_ERROR_INVALID_ARGUMENT if the stream is invalid.
     * ----------------------------------------------------------------------**/
    rmmError_t registerStream(cudaStream_t stream);

private:
    blazing_device_memory_resource() = default;
    ~blazing_device_memory_resource() = default;
    blazing_device_memory_resource(const blazing_device_memory_resource&) = delete;
    blazing_device_memory_resource& operator=(const blazing_device_memory_resource&) = delete;
    std::mutex manager_mutex;
    std::set<cudaStream_t> registered_streams;

    rmmOptions_t options{};
    bool is_initialized{false};

    std::unique_ptr<internal_blazing_device_memory_resource> initialized_resource{};
};

class blazing_host_memory_mesource : public BlazingMemoryResource{
public:
    static blazing_host_memory_mesource& getInstance(){
        // Myers' singleton. Thread safe and unique. Note: C++11 required.
        static blazing_host_memory_mesource instance;
        return instance;
    }
	// TODO: percy,cordova. Improve the design of get memory in real time 
	blazing_host_memory_mesource(float custom_threshold = 0.75) 
    {
		struct sysinfo si;
		sysinfo (&si);

		total_memory_size = (std::size_t)si.totalram;
		used_memory_size =  total_memory_size - (std::size_t)si.freeram;;
        memory_limit = custom_threshold * used_memory_size;
	}

	virtual ~blazing_host_memory_mesource() = default;

    // TODO
    void allocate(std::size_t bytes)  {
		used_memory_size +=  bytes;
	}

	void deallocate(std::size_t bytes)  {
		used_memory_size -= bytes;
	}

	std::size_t get_from_driver_available_memory()  {
        struct sysinfo si;
		sysinfo (&si);
        // NOTE: sync point 
		total_memory_size = (std::size_t)si.totalram;
		used_memory_size = total_memory_size - (std::size_t)si.freeram;;
        return used_memory_size;
    }

	std::size_t get_memory_used() override {
		return used_memory_size;
	}

	std::size_t get_total_memory() override {
		return total_memory_size;
	}

    std::size_t get_memory_limit() {
        // JUST FOR DEBUGGING: return 6586224;
        return memory_limit;
    }

private:
    std::size_t memory_limit;
	std::size_t total_memory_size;
	std::atomic<std::size_t> used_memory_size;
};


class blazing_disk_memory_resource : public  BlazingMemoryResource {
public:
    static blazing_disk_memory_resource& getInstance(){
        // Myers' singleton. Thread safe and unique. Note: C++11 required.
        static blazing_disk_memory_resource instance;
        return instance;
    }

	// TODO: percy, cordova.Improve the design of get memory in real time 
	blazing_disk_memory_resource(float custom_threshold = 0.75) {
		struct statvfs stat_disk;
		int ret = statvfs("/", &stat_disk);

		total_memory_size = (std::size_t)(stat_disk.f_blocks * stat_disk.f_frsize);
		std::size_t available_disk_size = (std::size_t)(stat_disk.f_bfree * stat_disk.f_frsize);
		used_memory_size = total_memory_size - available_disk_size;

        memory_limit = custom_threshold *  total_memory_size;
	}

	virtual ~blazing_disk_memory_resource() = default;

	virtual std::size_t get_from_driver_available_memory()  {
        struct sysinfo si;
        sysinfo (&si);
        // NOTE: sync point 
        total_memory_size = (std::size_t)si.totalram;
        used_memory_size =  total_memory_size - (std::size_t)si.freeram;
        return used_memory_size;
    }
	std::size_t get_memory_limit()  {
        return memory_limit;
    }

	std::size_t get_memory_used() {
        return used_memory_size;
	}

	std::size_t get_total_memory() {
		return total_memory_size;
	}

private:
	std::size_t total_memory_size;
    std::size_t memory_limit;
	std::atomic<std::size_t> used_memory_size;
};