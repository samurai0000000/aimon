/*
 * EdaAccelerators.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "EdaAccelerators.hxx"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <set>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace aimon {

namespace {

#if defined(_WIN32)
#define CL_API_CALL __stdcall
#else
#define CL_API_CALL
#endif

typedef int32_t cl_int;
typedef uint32_t cl_uint;
typedef uint64_t cl_ulong;
typedef uint64_t cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_uint cl_platform_info;
typedef cl_uint cl_device_info;
typedef cl_bitfield cl_device_fp_config;
typedef cl_bitfield cl_mem_flags;
typedef cl_uint cl_program_build_info;
typedef cl_int cl_build_status;

typedef struct _cl_platform_id* cl_platform_id;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_event* cl_event;
typedef struct _cl_sampler* cl_sampler;
typedef intptr_t cl_context_properties;

constexpr cl_int CL_SUCCESS = 0;
constexpr cl_int CL_DEVICE_NOT_FOUND = -1;
constexpr cl_device_type CL_DEVICE_TYPE_GPU = (1 << 2);

constexpr cl_device_info CL_DEVICE_TYPE = 0x1000;
constexpr cl_device_info CL_DEVICE_MAX_COMPUTE_UNITS = 0x1002;
constexpr cl_device_info CL_DEVICE_MAX_CLOCK_FREQUENCY = 0x100C;
constexpr cl_device_info CL_DEVICE_MAX_MEM_ALLOC_SIZE = 0x1010;
constexpr cl_device_info CL_DEVICE_GLOBAL_MEM_SIZE = 0x101F;
constexpr cl_device_info CL_DEVICE_NAME = 0x102B;
constexpr cl_device_info CL_DEVICE_VENDOR = 0x102C;
constexpr cl_device_info CL_DRIVER_VERSION = 0x102D;
constexpr cl_device_info CL_DEVICE_EXTENSIONS = 0x1030;

constexpr cl_mem_flags CL_MEM_READ_WRITE = (1 << 0);
constexpr cl_mem_flags CL_MEM_WRITE_ONLY = (1 << 1);
constexpr cl_mem_flags CL_MEM_READ_ONLY = (1 << 2);
constexpr cl_mem_flags CL_MEM_COPY_HOST_PTR = (1 << 5);

constexpr cl_context_properties CL_CONTEXT_PLATFORM = 0x1084;
constexpr cl_program_build_info CL_PROGRAM_BUILD_LOG = 0x1183;
constexpr cl_uint CL_TRUE = 1;
constexpr cl_uint CL_FALSE = 0;

using pfn_clGetPlatformIDs = cl_int (CL_API_CALL *)(cl_uint, cl_platform_id*, cl_uint*);
using pfn_clGetDeviceIDs = cl_int (CL_API_CALL *)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
using pfn_clGetDeviceInfo = cl_int (CL_API_CALL *)(cl_device_id, cl_device_info, size_t, void*, size_t*);
using pfn_clCreateContext = cl_context (CL_API_CALL *)(const cl_context_properties*, cl_uint, const cl_device_id*, void (CL_API_CALL *)(const char*, const void*, size_t, void*), void*, cl_int*);
using pfn_clCreateCommandQueue = cl_command_queue (CL_API_CALL *)(cl_context, cl_device_id, cl_bitfield, cl_int*);
using pfn_clCreateProgramWithSource = cl_program (CL_API_CALL *)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
using pfn_clBuildProgram = cl_int (CL_API_CALL *)(cl_program, cl_uint, const cl_device_id*, const char*, void (CL_API_CALL *)(cl_program, void*), void*);
using pfn_clGetProgramBuildInfo = cl_int (CL_API_CALL *)(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
using pfn_clCreateKernel = cl_kernel (CL_API_CALL *)(cl_program, const char*, cl_int*);
using pfn_clCreateBuffer = cl_mem (CL_API_CALL *)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
using pfn_clEnqueueWriteBuffer = cl_int (CL_API_CALL *)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void*, cl_uint, const cl_event*, cl_event*);
using pfn_clEnqueueReadBuffer = cl_int (CL_API_CALL *)(cl_command_queue, cl_mem, cl_uint, size_t, size_t, void*, cl_uint, const cl_event*, cl_event*);
using pfn_clSetKernelArg = cl_int (CL_API_CALL *)(cl_kernel, cl_uint, size_t, const void*);
using pfn_clEnqueueNDRangeKernel = cl_int (CL_API_CALL *)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
using pfn_clFinish = cl_int (CL_API_CALL *)(cl_command_queue);
using pfn_clReleaseMemObject = cl_int (CL_API_CALL *)(cl_mem);
using pfn_clReleaseKernel = cl_int (CL_API_CALL *)(cl_kernel);
using pfn_clReleaseProgram = cl_int (CL_API_CALL *)(cl_program);
using pfn_clReleaseCommandQueue = cl_int (CL_API_CALL *)(cl_command_queue);
using pfn_clReleaseContext = cl_int (CL_API_CALL *)(cl_context);

const char* kOpenClKernelSource = R"CLC(
__kernel void evalOrientationOverhangs(
    const int numCandidates,
    const float px, const float py, const float pz,
    const float hx, const float hy, const float hz,
    const float fMag, const float areaMm2,
    const float mMagNmm, const float secModMm3,
    const float sigmaZMpa, const float sigmaXyMpa,
    __global const float4* candidateNormals,
    __global float4* outMetrics
) {
    int gid = get_global_id(0);
    if (gid >= numCandidates) return;

    float4 c = candidateNormals[gid];
    float nx = c.x, ny = c.y, nz = c.z;
    float supportPenalty = c.w;

    float zAlign = fabs(px * nx + py * ny + pz * nz);
    float xyAlign = sqrt(fmax(0.0f, 1.0f - zAlign * zAlign));
    float normalStress = (fMag / areaMm2) + (mMagNmm / secModMm3);
    float zInterlayerStress = fmax(0.1f, normalStress * (0.25f + 0.75f * zAlign));
    float xyFiberStress = fmax(0.1f, normalStress * xyAlign);

    float delamSf = sigmaZMpa / zInterlayerStress;
    float fiberSf = sigmaXyMpa / xyFiberStress;
    float boreCircularity = fabs(hx * nx + hy * ny + hz * nz);
    float score = delamSf * 2.0f + boreCircularity * 1.5f - supportPenalty;

    outMetrics[gid] = (float4)(delamSf, fiberSf, boreCircularity, score);
}

__kernel void evalPlacementWirelength(
    const int numReplicas,
    const int numItems,
    const int numNets,
    const float clearanceMm,
    __global const float4* itemDims,
    __global const float2* replicaCoords,
    __global const int* netOffsets,
    __global const int* netMembers,
    __global float2* outCosts
) {
    int rep = get_global_id(0);
    if (rep >= numReplicas) return;

    int baseIdx = rep * numItems;
    float hpwl = 0.0f;

    for (int n = 0; n < numNets; ++n) {
        int start = netOffsets[n];
        int end = netOffsets[n + 1];
        if (start >= end) continue;
        int firstMember = netMembers[start];
        float minX = replicaCoords[baseIdx + firstMember].x;
        float maxX = minX;
        float minY = replicaCoords[baseIdx + firstMember].y;
        float maxY = minY;
        for (int m = start + 1; m < end; ++m) {
            int idx = netMembers[m];
            float x = replicaCoords[baseIdx + idx].x;
            float y = replicaCoords[baseIdx + idx].y;
            minX = fmin(minX, x);
            maxX = fmax(maxX, x);
            minY = fmin(minY, y);
            maxY = fmax(maxY, y);
        }
        hpwl += (maxX - minX) + (maxY - minY);
    }

    float overlapArea = 0.0f;
    for (int i = 0; i < numItems; ++i) {
        float4 dimI = itemDims[i];
        int rotI = (int)dimI.z;
        float wi = ((rotI % 180) != 0 ? dimI.y : dimI.x) * 0.5f + clearanceMm;
        float hi = ((rotI % 180) != 0 ? dimI.x : dimI.y) * 0.5f + clearanceMm;
        float xi = replicaCoords[baseIdx + i].x;
        float yi = replicaCoords[baseIdx + i].y;
        float layerI = dimI.w;

        for (int j = i + 1; j < numItems; ++j) {
            float4 dimJ = itemDims[j];
            if (layerI != dimJ.w) continue;
            int rotJ = (int)dimJ.z;
            float wj = ((rotJ % 180) != 0 ? dimJ.y : dimJ.x) * 0.5f + clearanceMm;
            float hj = ((rotJ % 180) != 0 ? dimJ.x : dimJ.y) * 0.5f + clearanceMm;
            float xj = replicaCoords[baseIdx + j].x;
            float yj = replicaCoords[baseIdx + j].y;

            float ox = (wi + wj) - fabs(xi - xj);
            float oy = (hi + hj) - fabs(yi - yj);
            if (ox > 0.0f && oy > 0.0f) {
                overlapArea += ox * oy;
            }
        }
    }

    outCosts[rep] = (float2)(hpwl + overlapArea * 500.0f, hpwl);
}

__kernel void rasterizeObstacleClearance(
    const int numQueries,
    const int numObstacles,
    __global const float4* queryPoints,
    __global const float4* obstacleBoxes,
    __global const int4* obstacleMeta,
    __global int* outClearanceFlags
) {
    int gid = get_global_id(0);
    if (gid >= numQueries) return;

    float4 q = queryPoints[gid];
    float qx = q.x;
    float qy = q.y;
    int qLayer = (int)q.z;
    int qNet = (int)q.w;

    int blocked = 0;
    for (int i = 0; i < numObstacles; ++i) {
        int4 meta = obstacleMeta[i];
        int obsLayer = meta.x;
        int obsNet = meta.y;
        if (obsLayer != -1 && qLayer != -1 && obsLayer != qLayer) continue;
        if (obsNet != 0 && obsNet == qNet) continue;

        float4 b = obstacleBoxes[i];
        if (meta.z == 1) {
            float dist = hypot(qx - b.x, qy - b.y);
            if (dist <= b.z) {
                blocked = 1;
                break;
            }
        } else {
            float ex = fmax(fabs(qx - b.x) - b.z, 0.0f);
            float ey = fmax(fabs(qy - b.y) - b.w, 0.0f);
            if (hypot(ex, ey) <= 0.001f) {
                blocked = 1;
                break;
            }
        }
    }
    outClearanceFlags[gid] = blocked;
}

__kernel void auditSegmentClearance(
    const int numSegments,
    const float minClearance,
    __global const float4* segStartEnd,
    __global const float4* segProps,
    __global int* outViolationCount,
    __global int2* outViolations,
    const int maxViolations
) {
    int i = get_global_id(0);
    if (i >= numSegments) return;

    float4 s1 = segStartEnd[i];
    float4 p1 = segProps[i];
    float w1 = p1.x;
    int layer1 = (int)p1.y;
    int net1 = (int)p1.z;

    for (int j = i + 1; j < numSegments; ++j) {
        float4 p2 = segProps[j];
        int layer2 = (int)p2.y;
        if (layer1 != -1 && layer2 != -1 && layer1 != layer2) continue;
        int net2 = (int)p2.z;
        if (net1 != 0 && net1 == net2) continue;

        float4 s2 = segStartEnd[j];
        float w2 = p2.x;
        float reqDist = 0.5f * (w1 + w2) + minClearance;

        float minX1 = fmin(s1.x, s1.z) - reqDist;
        float maxX1 = fmax(s1.x, s1.z) + reqDist;
        float minY1 = fmin(s1.y, s1.w) - reqDist;
        float maxY1 = fmax(s1.y, s1.w) + reqDist;

        float minX2 = fmin(s2.x, s2.z);
        float maxX2 = fmax(s2.x, s2.z);
        float minY2 = fmin(s2.y, s2.w);
        float maxY2 = fmax(s2.y, s2.w);

        if (maxX1 < minX2 || minX1 > maxX2 || maxY1 < minY2 || minY1 > maxY2) {
            continue;
        }

        float vx1 = s1.z - s1.x;
        float vy1 = s1.w - s1.y;
        float lenSq1 = vx1 * vx1 + vy1 * vy1;

        float vx2 = s2.z - s2.x;
        float vy2 = s2.w - s2.y;
        float lenSq2 = vx2 * vx2 + vy2 * vy2;

        float dMin = 1e9f;
        float t1 = (lenSq2 > 1e-12f) ? clamp(((s1.x - s2.x) * vx2 + (s1.y - s2.y) * vy2) / lenSq2, 0.0f, 1.0f) : 0.0f;
        dMin = fmin(dMin, hypot(s1.x - (s2.x + t1 * vx2), s1.y - (s2.y + t1 * vy2)));

        float t2 = (lenSq2 > 1e-12f) ? clamp(((s1.z - s2.x) * vx2 + (s1.w - s2.y) * vy2) / lenSq2, 0.0f, 1.0f) : 0.0f;
        dMin = fmin(dMin, hypot(s1.z - (s2.x + t2 * vx2), s1.w - (s2.y + t2 * vy2)));

        float t3 = (lenSq1 > 1e-12f) ? clamp(((s2.x - s1.x) * vx1 + (s2.y - s1.y) * vy1) / lenSq1, 0.0f, 1.0f) : 0.0f;
        dMin = fmin(dMin, hypot(s2.x - (s1.x + t3 * vx1), s2.y - (s1.y + t3 * vy1)));

        float t4 = (lenSq1 > 1e-12f) ? clamp(((s2.z - s1.x) * vx1 + (s2.w - s1.y) * vy1) / lenSq1, 0.0f, 1.0f) : 0.0f;
        dMin = fmin(dMin, hypot(s2.z - (s1.x + t4 * vx1), s2.w - (s1.y + t4 * vy1)));

        if (dMin < reqDist) {
            int slot = atomic_inc(outViolationCount);
            if (slot < maxViolations) {
                outViolations[slot] = (int2)(i, j);
            }
        }
    }
}
)CLC";

class OpenClEngine {
public:
    static OpenClEngine& instance() {
        static OpenClEngine s_instance;
        return s_instance;
    }

    bool isGpuAvailable() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _gpuAvailable;
    }

    AcceleratorCapabilities getCapabilities(bool enabled) const {
        std::lock_guard<std::mutex> lock(_mutex);
        AcceleratorCapabilities caps = _caps;
        caps.enabled = enabled;
        if (!enabled) {
            caps.capabilityScore = 0;
        }
        return caps;
    }

    bool runOrientationOverhangs(int numCandidates,
                                 float px, float py, float pz,
                                 float hx, float hy, float hz,
                                 float fMag, float areaMm2,
                                 float mMagNmm, float secModMm3,
                                 float sigmaZMpa, float sigmaXyMpa,
                                 const std::vector<float>& candidateData,
                                 std::vector<float>& outMetrics) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_gpuAvailable || !_queue || !_kPrintOrientation || numCandidates <= 0) {
            return false;
        }

        cl_int err = CL_SUCCESS;
        size_t inBytes = sizeof(float) * candidateData.size();
        size_t outBytes = sizeof(float) * 4 * static_cast<size_t>(numCandidates);
        outMetrics.resize(4 * static_cast<size_t>(numCandidates), 0.0f);

        cl_mem inBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, inBytes, const_cast<float*>(candidateData.data()), &err);
        if (err != CL_SUCCESS || !inBuf) return false;

        cl_mem outBuf = clCreateBuffer(_context, CL_MEM_WRITE_ONLY, outBytes, nullptr, &err);
        if (err != CL_SUCCESS || !outBuf) {
            clReleaseMemObject(inBuf);
            return false;
        }

        clSetKernelArg(_kPrintOrientation, 0, sizeof(int), &numCandidates);
        clSetKernelArg(_kPrintOrientation, 1, sizeof(float), &px);
        clSetKernelArg(_kPrintOrientation, 2, sizeof(float), &py);
        clSetKernelArg(_kPrintOrientation, 3, sizeof(float), &pz);
        clSetKernelArg(_kPrintOrientation, 4, sizeof(float), &hx);
        clSetKernelArg(_kPrintOrientation, 5, sizeof(float), &hy);
        clSetKernelArg(_kPrintOrientation, 6, sizeof(float), &hz);
        clSetKernelArg(_kPrintOrientation, 7, sizeof(float), &fMag);
        clSetKernelArg(_kPrintOrientation, 8, sizeof(float), &areaMm2);
        clSetKernelArg(_kPrintOrientation, 9, sizeof(float), &mMagNmm);
        clSetKernelArg(_kPrintOrientation, 10, sizeof(float), &secModMm3);
        clSetKernelArg(_kPrintOrientation, 11, sizeof(float), &sigmaZMpa);
        clSetKernelArg(_kPrintOrientation, 12, sizeof(float), &sigmaXyMpa);
        clSetKernelArg(_kPrintOrientation, 13, sizeof(cl_mem), &inBuf);
        clSetKernelArg(_kPrintOrientation, 14, sizeof(cl_mem), &outBuf);

        size_t globalWorkSize = static_cast<size_t>(numCandidates);
        err = clEnqueueNDRangeKernel(_queue, _kPrintOrientation, 1, nullptr, &globalWorkSize, nullptr, 0, nullptr, nullptr);
        if (err == CL_SUCCESS) {
            err = clEnqueueReadBuffer(_queue, outBuf, CL_TRUE, 0, outBytes, outMetrics.data(), 0, nullptr, nullptr);
        }

        clReleaseMemObject(inBuf);
        clReleaseMemObject(outBuf);
        return err == CL_SUCCESS;
    }

    bool runPlacementWirelength(int numReplicas, int numItems, int numNets,
                                float clearanceMm,
                                const std::vector<float>& itemDims,
                                const std::vector<float>& replicaCoords,
                                const std::vector<int>& netOffsets,
                                const std::vector<int>& netMembers,
                                std::vector<float>& outCosts) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_gpuAvailable || !_queue || !_kPcbPlacement || numReplicas <= 0 || numItems <= 0) {
            return false;
        }

        cl_int err = CL_SUCCESS;
        size_t dimsBytes = sizeof(float) * itemDims.size();
        size_t coordsBytes = sizeof(float) * replicaCoords.size();
        size_t offsetsBytes = sizeof(int) * netOffsets.size();
        size_t membersBytes = sizeof(int) * netMembers.size();
        size_t outBytes = sizeof(float) * 2 * static_cast<size_t>(numReplicas);
        outCosts.resize(2 * static_cast<size_t>(numReplicas), 0.0f);

        cl_mem dimsBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, dimsBytes, const_cast<float*>(itemDims.data()), &err);
        if (err != CL_SUCCESS || !dimsBuf) return false;
        cl_mem coordsBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, coordsBytes, const_cast<float*>(replicaCoords.data()), &err);
        if (err != CL_SUCCESS || !coordsBuf) { clReleaseMemObject(dimsBuf); return false; }
        cl_mem offsetsBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, offsetsBytes, const_cast<int*>(netOffsets.data()), &err);
        if (err != CL_SUCCESS || !offsetsBuf) { clReleaseMemObject(dimsBuf); clReleaseMemObject(coordsBuf); return false; }
        cl_mem membersBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, membersBytes, const_cast<int*>(netMembers.data()), &err);
        if (err != CL_SUCCESS || !membersBuf) { clReleaseMemObject(dimsBuf); clReleaseMemObject(coordsBuf); clReleaseMemObject(offsetsBuf); return false; }
        cl_mem outBuf = clCreateBuffer(_context, CL_MEM_WRITE_ONLY, outBytes, nullptr, &err);
        if (err != CL_SUCCESS || !outBuf) { clReleaseMemObject(dimsBuf); clReleaseMemObject(coordsBuf); clReleaseMemObject(offsetsBuf); clReleaseMemObject(membersBuf); return false; }

        clSetKernelArg(_kPcbPlacement, 0, sizeof(int), &numReplicas);
        clSetKernelArg(_kPcbPlacement, 1, sizeof(int), &numItems);
        clSetKernelArg(_kPcbPlacement, 2, sizeof(int), &numNets);
        clSetKernelArg(_kPcbPlacement, 3, sizeof(float), &clearanceMm);
        clSetKernelArg(_kPcbPlacement, 4, sizeof(cl_mem), &dimsBuf);
        clSetKernelArg(_kPcbPlacement, 5, sizeof(cl_mem), &coordsBuf);
        clSetKernelArg(_kPcbPlacement, 6, sizeof(cl_mem), &offsetsBuf);
        clSetKernelArg(_kPcbPlacement, 7, sizeof(cl_mem), &membersBuf);
        clSetKernelArg(_kPcbPlacement, 8, sizeof(cl_mem), &outBuf);

        size_t globalWorkSize = static_cast<size_t>(numReplicas);
        err = clEnqueueNDRangeKernel(_queue, _kPcbPlacement, 1, nullptr, &globalWorkSize, nullptr, 0, nullptr, nullptr);
        if (err == CL_SUCCESS) {
            err = clEnqueueReadBuffer(_queue, outBuf, CL_TRUE, 0, outBytes, outCosts.data(), 0, nullptr, nullptr);
        }

        clReleaseMemObject(dimsBuf);
        clReleaseMemObject(coordsBuf);
        clReleaseMemObject(offsetsBuf);
        clReleaseMemObject(membersBuf);
        clReleaseMemObject(outBuf);
        return err == CL_SUCCESS;
    }

    bool runObstacleClearance(int numQueries, int numObstacles,
                              const std::vector<float>& queries,
                              const std::vector<float>& boxes,
                              const std::vector<int>& meta,
                              std::vector<int>& outFlags) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_gpuAvailable || !_queue || !_kPcbAutoroute || numQueries <= 0 || numObstacles <= 0) {
            return false;
        }

        cl_int err = CL_SUCCESS;
        size_t qBytes = sizeof(float) * queries.size();
        size_t bBytes = sizeof(float) * boxes.size();
        size_t mBytes = sizeof(int) * meta.size();
        size_t outBytes = sizeof(int) * static_cast<size_t>(numQueries);
        outFlags.resize(static_cast<size_t>(numQueries), 0);

        cl_mem qBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, qBytes, const_cast<float*>(queries.data()), &err);
        if (err != CL_SUCCESS || !qBuf) return false;
        cl_mem bBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, bBytes, const_cast<float*>(boxes.data()), &err);
        if (err != CL_SUCCESS || !bBuf) { clReleaseMemObject(qBuf); return false; }
        cl_mem mBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, mBytes, const_cast<int*>(meta.data()), &err);
        if (err != CL_SUCCESS || !mBuf) { clReleaseMemObject(qBuf); clReleaseMemObject(bBuf); return false; }
        cl_mem outBuf = clCreateBuffer(_context, CL_MEM_WRITE_ONLY, outBytes, nullptr, &err);
        if (err != CL_SUCCESS || !outBuf) { clReleaseMemObject(qBuf); clReleaseMemObject(bBuf); clReleaseMemObject(mBuf); return false; }

        clSetKernelArg(_kPcbAutoroute, 0, sizeof(int), &numQueries);
        clSetKernelArg(_kPcbAutoroute, 1, sizeof(int), &numObstacles);
        clSetKernelArg(_kPcbAutoroute, 2, sizeof(cl_mem), &qBuf);
        clSetKernelArg(_kPcbAutoroute, 3, sizeof(cl_mem), &bBuf);
        clSetKernelArg(_kPcbAutoroute, 4, sizeof(cl_mem), &mBuf);
        clSetKernelArg(_kPcbAutoroute, 5, sizeof(cl_mem), &outBuf);

        size_t globalWorkSize = static_cast<size_t>(numQueries);
        err = clEnqueueNDRangeKernel(_queue, _kPcbAutoroute, 1, nullptr, &globalWorkSize, nullptr, 0, nullptr, nullptr);
        if (err == CL_SUCCESS) {
            err = clEnqueueReadBuffer(_queue, outBuf, CL_TRUE, 0, outBytes, outFlags.data(), 0, nullptr, nullptr);
        }

        clReleaseMemObject(qBuf);
        clReleaseMemObject(bBuf);
        clReleaseMemObject(mBuf);
        clReleaseMemObject(outBuf);
        return err == CL_SUCCESS;
    }

    bool runSegmentClearance(int numSegments, float minClearance,
                             const std::vector<float>& segStartEnd,
                             const std::vector<float>& segProps,
                             int& outViolationCount,
                             std::vector<int>& outViolations) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_gpuAvailable || !_queue || !_kPcbConnectivityAudit || numSegments <= 1) {
            return false;
        }

        cl_int err = CL_SUCCESS;
        int maxViolations = 4096;
        outViolations.resize(2 * static_cast<size_t>(maxViolations), 0);
        outViolationCount = 0;

        size_t seBytes = sizeof(float) * segStartEnd.size();
        size_t pBytes = sizeof(float) * segProps.size();
        size_t countBytes = sizeof(int);
        size_t violBytes = sizeof(int) * 2 * static_cast<size_t>(maxViolations);

        cl_mem seBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, seBytes, const_cast<float*>(segStartEnd.data()), &err);
        if (err != CL_SUCCESS || !seBuf) return false;
        cl_mem pBuf = clCreateBuffer(_context, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR, pBytes, const_cast<float*>(segProps.data()), &err);
        if (err != CL_SUCCESS || !pBuf) { clReleaseMemObject(seBuf); return false; }
        cl_mem countBuf = clCreateBuffer(_context, CL_MEM_READ_WRITE | CL_MEM_COPY_HOST_PTR, countBytes, &outViolationCount, &err);
        if (err != CL_SUCCESS || !countBuf) { clReleaseMemObject(seBuf); clReleaseMemObject(pBuf); return false; }
        cl_mem violBuf = clCreateBuffer(_context, CL_MEM_WRITE_ONLY, violBytes, nullptr, &err);
        if (err != CL_SUCCESS || !violBuf) { clReleaseMemObject(seBuf); clReleaseMemObject(pBuf); clReleaseMemObject(countBuf); return false; }

        clSetKernelArg(_kPcbConnectivityAudit, 0, sizeof(int), &numSegments);
        clSetKernelArg(_kPcbConnectivityAudit, 1, sizeof(float), &minClearance);
        clSetKernelArg(_kPcbConnectivityAudit, 2, sizeof(cl_mem), &seBuf);
        clSetKernelArg(_kPcbConnectivityAudit, 3, sizeof(cl_mem), &pBuf);
        clSetKernelArg(_kPcbConnectivityAudit, 4, sizeof(cl_mem), &countBuf);
        clSetKernelArg(_kPcbConnectivityAudit, 5, sizeof(cl_mem), &violBuf);
        clSetKernelArg(_kPcbConnectivityAudit, 6, sizeof(int), &maxViolations);

        size_t globalWorkSize = static_cast<size_t>(numSegments);
        err = clEnqueueNDRangeKernel(_queue, _kPcbConnectivityAudit, 1, nullptr, &globalWorkSize, nullptr, 0, nullptr, nullptr);
        if (err == CL_SUCCESS) {
            clEnqueueReadBuffer(_queue, countBuf, CL_TRUE, 0, countBytes, &outViolationCount, 0, nullptr, nullptr);
            int readViolations = std::min(outViolationCount, maxViolations);
            if (readViolations > 0) {
                clEnqueueReadBuffer(_queue, violBuf, CL_TRUE, 0, sizeof(int) * 2 * static_cast<size_t>(readViolations), outViolations.data(), 0, nullptr, nullptr);
            }
        }

        clReleaseMemObject(seBuf);
        clReleaseMemObject(pBuf);
        clReleaseMemObject(countBuf);
        clReleaseMemObject(violBuf);
        return err == CL_SUCCESS;
    }

private:
    OpenClEngine() {
        init();
    }

    ~OpenClEngine() {
        cleanup();
    }

    void initDefaultCpuCaps() {
        _gpuAvailable = false;
        _caps.enabled = true;
        _caps.gpuAvailable = false;
        _caps.backend = "cpu_cpp17";
        _caps.deviceName = "CPU C++17 Solver";
        _caps.vendor = "Host CPU";
        _caps.driverVersion = "";
        _caps.computeUnits = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        _caps.clockMhz = 0;
        _caps.globalMemBytes = 0;
        _caps.maxAllocBytes = 0;
        _caps.fp16Supported = false;
        _caps.fp64Supported = false;
        _caps.cpuThreads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
        _caps.gpuAcceleratedTools.clear();
        _caps.capabilityScore = std::max<int64_t>(100, static_cast<int64_t>(_caps.cpuThreads) * 100);
    }

    template <typename T>
    bool loadSymbol(T& fn, const char* name) {
#ifdef _WIN32
        fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(_lib, name)));
#else
        fn = reinterpret_cast<T>(dlsym(_lib, name));
#endif
        return fn != nullptr;
    }

    void cleanup() {
        if (_kPrintOrientation && clReleaseKernel) { clReleaseKernel(_kPrintOrientation); _kPrintOrientation = nullptr; }
        if (_kPcbPlacement && clReleaseKernel) { clReleaseKernel(_kPcbPlacement); _kPcbPlacement = nullptr; }
        if (_kPcbAutoroute && clReleaseKernel) { clReleaseKernel(_kPcbAutoroute); _kPcbAutoroute = nullptr; }
        if (_kPcbConnectivityAudit && clReleaseKernel) { clReleaseKernel(_kPcbConnectivityAudit); _kPcbConnectivityAudit = nullptr; }
        if (_program && clReleaseProgram) { clReleaseProgram(_program); _program = nullptr; }
        if (_queue && clReleaseCommandQueue) { clReleaseCommandQueue(_queue); _queue = nullptr; }
        if (_context && clReleaseContext) { clReleaseContext(_context); _context = nullptr; }
        if (_lib) {
#ifdef _WIN32
            FreeLibrary(_lib);
#else
            dlclose(_lib);
#endif
            _lib = nullptr;
        }
    }

    void init() {
        initDefaultCpuCaps();
#ifdef _WIN32
        _lib = LoadLibraryW(L"OpenCL.dll");
#else
        _lib = dlopen("libOpenCL.so.1", RTLD_LAZY);
        if (!_lib) {
            _lib = dlopen("libOpenCL.so", RTLD_LAZY);
        }
#endif
        if (!_lib) {
            return;
        }

        bool ok = true;
        ok = ok && loadSymbol(clGetPlatformIDs, "clGetPlatformIDs");
        ok = ok && loadSymbol(clGetDeviceIDs, "clGetDeviceIDs");
        ok = ok && loadSymbol(clGetDeviceInfo, "clGetDeviceInfo");
        ok = ok && loadSymbol(clCreateContext, "clCreateContext");
        ok = ok && loadSymbol(clCreateCommandQueue, "clCreateCommandQueue");
        ok = ok && loadSymbol(clCreateProgramWithSource, "clCreateProgramWithSource");
        ok = ok && loadSymbol(clBuildProgram, "clBuildProgram");
        ok = ok && loadSymbol(clGetProgramBuildInfo, "clGetProgramBuildInfo");
        ok = ok && loadSymbol(clCreateKernel, "clCreateKernel");
        ok = ok && loadSymbol(clCreateBuffer, "clCreateBuffer");
        ok = ok && loadSymbol(clEnqueueWriteBuffer, "clEnqueueWriteBuffer");
        ok = ok && loadSymbol(clEnqueueReadBuffer, "clEnqueueReadBuffer");
        ok = ok && loadSymbol(clSetKernelArg, "clSetKernelArg");
        ok = ok && loadSymbol(clEnqueueNDRangeKernel, "clEnqueueNDRangeKernel");
        ok = ok && loadSymbol(clFinish, "clFinish");
        ok = ok && loadSymbol(clReleaseMemObject, "clReleaseMemObject");
        ok = ok && loadSymbol(clReleaseKernel, "clReleaseKernel");
        ok = ok && loadSymbol(clReleaseProgram, "clReleaseProgram");
        ok = ok && loadSymbol(clReleaseCommandQueue, "clReleaseCommandQueue");
        ok = ok && loadSymbol(clReleaseContext, "clReleaseContext");

        if (!ok) {
            cleanup();
            return;
        }

        cl_uint numPlatforms = 0;
        if (clGetPlatformIDs(0, nullptr, &numPlatforms) != CL_SUCCESS || numPlatforms == 0) {
            cleanup();
            return;
        }

        std::vector<cl_platform_id> platforms(numPlatforms);
        if (clGetPlatformIDs(numPlatforms, platforms.data(), nullptr) != CL_SUCCESS) {
            cleanup();
            return;
        }

        cl_platform_id bestPlatform = nullptr;
        cl_device_id bestDevice = nullptr;
        int64_t bestDeviceScore = -1;
        AcceleratorCapabilities bestCaps;

        for (cl_platform_id plat : platforms) {
            cl_uint numDevices = 0;
            if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 0, nullptr, &numDevices) != CL_SUCCESS || numDevices == 0) {
                continue;
            }
            std::vector<cl_device_id> devices(numDevices);
            if (clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, numDevices, devices.data(), nullptr) != CL_SUCCESS) {
                continue;
            }

            for (cl_device_id dev : devices) {
                auto queryStr = [this, dev](cl_device_info info) -> std::string {
                    size_t size = 0;
                    if (clGetDeviceInfo(dev, info, 0, nullptr, &size) != CL_SUCCESS || size == 0) return "";
                    std::string s(size, '\0');
                    if (clGetDeviceInfo(dev, info, size, &s[0], nullptr) != CL_SUCCESS) return "";
                    while (!s.empty() && (s.back() == '\0' || s.back() == ' ')) s.pop_back();
                    return s;
                };

                auto queryUint = [this, dev](cl_device_info info, cl_uint defVal) -> cl_uint {
                    cl_uint val = defVal;
                    clGetDeviceInfo(dev, info, sizeof(val), &val, nullptr);
                    return val;
                };

                auto queryUlong = [this, dev](cl_device_info info, cl_ulong defVal) -> cl_ulong {
                    cl_ulong val = defVal;
                    clGetDeviceInfo(dev, info, sizeof(val), &val, nullptr);
                    return val;
                };

                std::string name = queryStr(CL_DEVICE_NAME);
                std::string vendor = queryStr(CL_DEVICE_VENDOR);
                std::string driverVer = queryStr(CL_DRIVER_VERSION);
                cl_uint computeUnits = queryUint(CL_DEVICE_MAX_COMPUTE_UNITS, 1);
                cl_uint clockMhz = queryUint(CL_DEVICE_MAX_CLOCK_FREQUENCY, 1000);
                cl_ulong globalMem = queryUlong(CL_DEVICE_GLOBAL_MEM_SIZE, 0);
                cl_ulong maxAlloc = queryUlong(CL_DEVICE_MAX_MEM_ALLOC_SIZE, 0);
                std::string extensions = queryStr(CL_DEVICE_EXTENSIONS);
                bool fp16 = extensions.find("cl_khr_fp16") != std::string::npos;
                bool fp64 = extensions.find("cl_khr_fp64") != std::string::npos;

                int64_t score = static_cast<int64_t>(computeUnits) * static_cast<int64_t>(std::max<cl_uint>(clockMhz, 1000)) +
                                static_cast<int64_t>(globalMem / (1024ULL * 1024ULL));

                if (score > bestDeviceScore) {
                    bestDeviceScore = score;
                    bestPlatform = plat;
                    bestDevice = dev;

                    bestCaps.enabled = true;
                    bestCaps.gpuAvailable = true;
                    bestCaps.backend = "opencl_gpu";
                    bestCaps.deviceName = name.empty() ? "OpenCL GPU" : name;
                    bestCaps.vendor = vendor.empty() ? "GPU Vendor" : vendor;
                    bestCaps.driverVersion = driverVer;
                    bestCaps.computeUnits = static_cast<int>(computeUnits);
                    bestCaps.clockMhz = static_cast<int>(clockMhz);
                    bestCaps.globalMemBytes = globalMem;
                    bestCaps.maxAllocBytes = maxAlloc;
                    bestCaps.fp16Supported = fp16;
                    bestCaps.fp64Supported = fp64;
                    bestCaps.cpuThreads = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
                    bestCaps.capabilityScore = 100000LL +
                        static_cast<int64_t>(std::max(1, bestCaps.computeUnits)) * static_cast<int64_t>(std::max(bestCaps.clockMhz, 1000)) +
                        static_cast<int64_t>(bestCaps.globalMemBytes / (1024ULL * 1024ULL));
                    bestCaps.gpuAcceleratedTools = {
                        "cad_calc_print_orientation_dfm",
                        "eda_calc_pcb_placement",
                        "eda_calc_pcb_autoroute",
                        "eda_calc_pcb_connectivity_audit"
                    };
                }
            }
        }

        if (!bestDevice) {
            cleanup();
            return;
        }

        cl_int clErr = CL_SUCCESS;
        cl_context_properties props[] = {
            CL_CONTEXT_PLATFORM,
            reinterpret_cast<cl_context_properties>(bestPlatform),
            0
        };
        _context = clCreateContext(props, 1, &bestDevice, nullptr, nullptr, &clErr);
        if (clErr != CL_SUCCESS || !_context) {
            cleanup();
            return;
        }

        _queue = clCreateCommandQueue(_context, bestDevice, 0, &clErr);
        if (clErr != CL_SUCCESS || !_queue) {
            cleanup();
            return;
        }

        const char* srcPtr = kOpenClKernelSource;
        size_t srcLen = std::strlen(kOpenClKernelSource);
        _program = clCreateProgramWithSource(_context, 1, &srcPtr, &srcLen, &clErr);
        if (clErr != CL_SUCCESS || !_program) {
            cleanup();
            return;
        }

        clErr = clBuildProgram(_program, 1, &bestDevice, "-cl-fast-relaxed-math", nullptr, nullptr);
        if (clErr != CL_SUCCESS) {
            cleanup();
            return;
        }

        _kPrintOrientation = clCreateKernel(_program, "evalOrientationOverhangs", &clErr);
        if (clErr != CL_SUCCESS || !_kPrintOrientation) {
            cleanup();
            return;
        }

        _kPcbPlacement = clCreateKernel(_program, "evalPlacementWirelength", &clErr);
        if (clErr != CL_SUCCESS || !_kPcbPlacement) {
            cleanup();
            return;
        }

        _kPcbAutoroute = clCreateKernel(_program, "rasterizeObstacleClearance", &clErr);
        if (clErr != CL_SUCCESS || !_kPcbAutoroute) {
            cleanup();
            return;
        }

        _kPcbConnectivityAudit = clCreateKernel(_program, "auditSegmentClearance", &clErr);
        if (clErr != CL_SUCCESS || !_kPcbConnectivityAudit) {
            cleanup();
            return;
        }

        _caps = bestCaps;
        _gpuAvailable = true;
    }

    mutable std::mutex _mutex;
#ifdef _WIN32
    HMODULE _lib = nullptr;
#else
    void* _lib = nullptr;
#endif
    bool _gpuAvailable = false;
    AcceleratorCapabilities _caps;

    cl_context _context = nullptr;
    cl_command_queue _queue = nullptr;
    cl_program _program = nullptr;
    cl_kernel _kPrintOrientation = nullptr;
    cl_kernel _kPcbPlacement = nullptr;
    cl_kernel _kPcbAutoroute = nullptr;
    cl_kernel _kPcbConnectivityAudit = nullptr;

    pfn_clGetPlatformIDs clGetPlatformIDs = nullptr;
    pfn_clGetDeviceIDs clGetDeviceIDs = nullptr;
    pfn_clGetDeviceInfo clGetDeviceInfo = nullptr;
    pfn_clCreateContext clCreateContext = nullptr;
    pfn_clCreateCommandQueue clCreateCommandQueue = nullptr;
    pfn_clCreateProgramWithSource clCreateProgramWithSource = nullptr;
    pfn_clBuildProgram clBuildProgram = nullptr;
    pfn_clGetProgramBuildInfo clGetProgramBuildInfo = nullptr;
    pfn_clCreateKernel clCreateKernel = nullptr;
    pfn_clCreateBuffer clCreateBuffer = nullptr;
    pfn_clEnqueueWriteBuffer clEnqueueWriteBuffer = nullptr;
    pfn_clEnqueueReadBuffer clEnqueueReadBuffer = nullptr;
    pfn_clSetKernelArg clSetKernelArg = nullptr;
    pfn_clEnqueueNDRangeKernel clEnqueueNDRangeKernel = nullptr;
    pfn_clFinish clFinish = nullptr;
    pfn_clReleaseMemObject clReleaseMemObject = nullptr;
    pfn_clReleaseKernel clReleaseKernel = nullptr;
    pfn_clReleaseProgram clReleaseProgram = nullptr;
    pfn_clReleaseCommandQueue clReleaseCommandQueue = nullptr;
    pfn_clReleaseContext clReleaseContext = nullptr;
};

struct AutoInitOpenCl {
    AutoInitOpenCl() {
        OpenClEngine::instance();
    }
} s_autoInitOpenCl;

constexpr double kPi = 3.14159265358979323846;
constexpr double kGravity = 9.80665;
constexpr double kSchematicGridMm = 1.27; // 50 mils

double snapTo50Mil(double valMm) {
    return std::llround(valMm / kSchematicGridMm) * kSchematicGridMm;
}

int toGridCoord(double valMm, double stepMm) {
    return static_cast<int>(std::llround(valMm / stepMm));
}

double fromGridCoord(int idx, double stepMm) {
    return static_cast<double>(idx) * stepMm;
}

constexpr double kMmPerMil = 0.0254;
constexpr double kRouteMarginMm = 2.5;            // 100 mil routing margin around copper bbox
constexpr int64_t kMaxRouteCellsPerLayer = 640000; // 0.10 mm grid on an 80 x 80 mm board
constexpr int32_t kCellBlockAll = -1;              // obstacle / keepout / multi-net halo overlap

double pointSegmentDistance(double px, double py, double ax, double ay, double bx, double by) {
    double vx = bx - ax;
    double vy = by - ay;
    double len2 = vx * vx + vy * vy;
    double t = 0.0;
    if (len2 > 1e-18) {
        t = std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.0, 1.0);
    }
    return std::hypot(px - (ax + t * vx), py - (ay + t * vy));
}

void closestPointOnSegment(double px, double py, double ax, double ay, double bx, double by,
                           double& outX, double& outY) {
    double vx = bx - ax;
    double vy = by - ay;
    double len2 = vx * vx + vy * vy;
    double t = 0.0;
    if (len2 > 1e-18) {
        t = std::clamp(((px - ax) * vx + (py - ay) * vy) / len2, 0.0, 1.0);
    }
    outX = ax + t * vx;
    outY = ay + t * vy;
}

// Proper (crossing) intersection only; touching/collinear cases yield distance 0 through the
// endpoint-to-segment checks in segmentSegmentDistance().
bool segmentsCross(double ax, double ay, double bx, double by,
                   double cx, double cy, double dx, double dy) {
    auto cross = [](double ux, double uy, double vx, double vy) { return ux * vy - uy * vx; };
    double d1 = cross(dx - cx, dy - cy, ax - cx, ay - cy);
    double d2 = cross(dx - cx, dy - cy, bx - cx, by - cy);
    double d3 = cross(bx - ax, by - ay, cx - ax, cy - ay);
    double d4 = cross(bx - ax, by - ay, dx - ax, dy - ay);
    return ((d1 > 0.0 && d2 < 0.0) || (d1 < 0.0 && d2 > 0.0)) &&
           ((d3 > 0.0 && d4 < 0.0) || (d3 < 0.0 && d4 > 0.0));
}

double segmentSegmentDistance(double ax, double ay, double bx, double by,
                              double cx, double cy, double dx, double dy) {
    if (segmentsCross(ax, ay, bx, by, cx, cy, dx, dy)) return 0.0;
    return std::min({pointSegmentDistance(ax, ay, cx, cy, dx, dy),
                     pointSegmentDistance(bx, by, cx, cy, dx, dy),
                     pointSegmentDistance(cx, cy, ax, ay, bx, by),
                     pointSegmentDistance(dx, dy, ax, ay, bx, by)});
}

// Euclidean distance from a point (in the rectangle's local frame) to an axis-aligned
// rectangle centred at the origin with half extents (hw, hh). Zero inside.
double pointRectDistanceLocal(double lx, double ly, double hw, double hh) {
    double ex = std::max(std::fabs(lx) - hw, 0.0);
    double ey = std::max(std::fabs(ly) - hh, 0.0);
    return std::hypot(ex, ey);
}

double segmentRectDistanceLocal(double ax, double ay, double bx, double by, double hw, double hh) {
    double best = std::min(pointRectDistanceLocal(ax, ay, hw, hh), pointRectDistanceLocal(bx, by, hw, hh));
    if (best <= 0.0) return 0.0;
    const double cxs[4] = {-hw, hw, hw, -hw};
    const double cys[4] = {-hh, -hh, hh, hh};
    for (int i = 0; i < 4; ++i) {
        int j = (i + 1) % 4;
        best = std::min(best, segmentSegmentDistance(ax, ay, bx, by, cxs[i], cys[i], cxs[j], cys[j]));
    }
    return best;
}

struct DisjointSet {
    std::vector<int> parent;
    explicit DisjointSet(size_t n) : parent(n) {
        std::iota(parent.begin(), parent.end(), 0);
    }
    int find(int a) {
        while (parent[static_cast<size_t>(a)] != a) {
            parent[static_cast<size_t>(a)] = parent[static_cast<size_t>(parent[static_cast<size_t>(a)])];
            a = parent[static_cast<size_t>(a)];
        }
        return a;
    }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[static_cast<size_t>(b)] = a;
    }
};

int layerPriorityForCategory(const std::string& cat) {
    if (cat == "connector" || cat == "input" || cat == "sensor") return 0;
    if (cat == "power" || cat == "regulator" || cat == "filter") return 1;
    if (cat == "mcu" || cat == "core" || cat == "logic" || cat == "ic") return 2;
    if (cat == "driver" || cat == "actuator" || cat == "output") return 3;
    return 2;
}

constexpr double kE24Basic[] = {
    1.0, 1.2, 1.5, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0, 3.3, 3.6, 3.9,
    4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1
};
constexpr size_t kE24BasicLen = sizeof(kE24Basic) / sizeof(kE24Basic[0]);

constexpr double kE24Full[] = {
    1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
    3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1
};
constexpr size_t kE24FullLen = sizeof(kE24Full) / sizeof(kE24Full[0]);

constexpr double kE96[] = {
    1.00, 1.02, 1.05, 1.07, 1.10, 1.13, 1.15, 1.18, 1.21, 1.24, 1.27, 1.30,
    1.33, 1.37, 1.40, 1.43, 1.47, 1.50, 1.54, 1.58, 1.62, 1.65, 1.69, 1.74,
    1.78, 1.82, 1.87, 1.91, 1.96, 2.00, 2.05, 2.10, 2.15, 2.21, 2.26, 2.32,
    2.37, 2.43, 2.49, 2.55, 2.61, 2.67, 2.74, 2.80, 2.87, 2.94, 3.01, 3.09,
    3.16, 3.24, 3.32, 3.40, 3.48, 3.57, 3.65, 3.74, 3.83, 3.92, 4.02, 4.12,
    4.22, 4.32, 4.42, 4.53, 4.64, 4.75, 4.87, 4.99, 5.11, 5.23, 5.36, 5.49,
    5.62, 5.76, 5.90, 6.04, 6.19, 6.34, 6.49, 6.65, 6.81, 6.98, 7.15, 7.32,
    7.50, 7.68, 7.87, 8.06, 8.25, 8.45, 8.66, 8.87, 9.09, 9.31, 9.53, 9.76
};
constexpr size_t kE96Len = sizeof(kE96) / sizeof(kE96[0]);

void getESeriesBase(const std::string& series, bool jlcpcbBasic,
                    const double*& outPtr, size_t& outLen) {
    if (series == "E96") {
        outPtr = kE96;
        outLen = kE96Len;
    } else if (jlcpcbBasic) {
        outPtr = kE24Basic;
        outLen = kE24BasicLen;
    } else {
        outPtr = kE24Full;
        outLen = kE24FullLen;
    }
}

} // anonymous namespace

int64_t AcceleratorCapabilities::scoreForTool(const std::string& toolName) const {
    if (!enabled) {
        return 0;
    }
    int64_t cpuScore = std::max<int64_t>(100, static_cast<int64_t>(std::max(1, cpuThreads)) * 100);
    if (!gpuAvailable) {
        return std::max(capabilityScore, cpuScore);
    }
    int64_t fullGpuScore = 100000LL +
        static_cast<int64_t>(std::max(1, computeUnits)) * static_cast<int64_t>(std::max(clockMhz, 1000)) +
        static_cast<int64_t>(globalMemBytes / (1024ULL * 1024ULL));
    return std::max(capabilityScore, fullGpuScore);
}

nlohmann::json AcceleratorCapabilities::toJson() const {
    return {
        {"enabled", enabled},
        {"gpu_available", gpuAvailable},
        {"backend", backend},
        {"device_name", deviceName},
        {"vendor", vendor},
        {"driver_version", driverVersion},
        {"compute_units", computeUnits},
        {"clock_mhz", clockMhz},
        {"global_mem_bytes", globalMemBytes},
        {"max_alloc_bytes", maxAllocBytes},
        {"fp16_supported", fp16Supported},
        {"fp64_supported", fp64Supported},
        {"cpu_threads", cpuThreads},
        {"capability_score", capabilityScore},
        {"gpu_accelerated_tools", gpuAcceleratedTools}
    };
}

AcceleratorCapabilities AcceleratorCapabilities::fromJson(const nlohmann::json& j) {
    AcceleratorCapabilities caps;
    if (!j.is_object()) {
        if (caps.capabilityScore <= 0) {
            caps.capabilityScore = caps.scoreForTool("");
        }
        return caps;
    }

    if (j.contains("enabled") && j["enabled"].is_boolean()) {
        caps.enabled = j["enabled"].get<bool>();
    }
    if (j.contains("gpu_available") && j["gpu_available"].is_boolean()) {
        caps.gpuAvailable = j["gpu_available"].get<bool>();
    }
    if (j.contains("backend") && j["backend"].is_string()) {
        caps.backend = j["backend"].get<std::string>();
    }
    if (j.contains("device_name") && j["device_name"].is_string()) {
        caps.deviceName = j["device_name"].get<std::string>();
    }
    if (j.contains("vendor") && j["vendor"].is_string()) {
        caps.vendor = j["vendor"].get<std::string>();
    }
    if (j.contains("driver_version") && j["driver_version"].is_string()) {
        caps.driverVersion = j["driver_version"].get<std::string>();
    }
    if (j.contains("compute_units") && j["compute_units"].is_number()) {
        caps.computeUnits = std::max(1, static_cast<int>(j["compute_units"].get<double>()));
    }
    if (j.contains("clock_mhz") && j["clock_mhz"].is_number()) {
        caps.clockMhz = std::max(0, static_cast<int>(j["clock_mhz"].get<double>()));
    }
    if (j.contains("global_mem_bytes") && j["global_mem_bytes"].is_number()) {
        double v = j["global_mem_bytes"].get<double>();
        caps.globalMemBytes = (v > 0.0) ? static_cast<uint64_t>(v) : 0ULL;
    }
    if (j.contains("max_alloc_bytes") && j["max_alloc_bytes"].is_number()) {
        double v = j["max_alloc_bytes"].get<double>();
        caps.maxAllocBytes = (v > 0.0) ? static_cast<uint64_t>(v) : 0ULL;
    }
    if (j.contains("fp16_supported") && j["fp16_supported"].is_boolean()) {
        caps.fp16Supported = j["fp16_supported"].get<bool>();
    }
    if (j.contains("fp64_supported") && j["fp64_supported"].is_boolean()) {
        caps.fp64Supported = j["fp64_supported"].get<bool>();
    }
    if (j.contains("cpu_threads") && j["cpu_threads"].is_number()) {
        caps.cpuThreads = std::max(1, static_cast<int>(j["cpu_threads"].get<double>()));
    }
    if (j.contains("gpu_accelerated_tools") && j["gpu_accelerated_tools"].is_array()) {
        caps.gpuAcceleratedTools.clear();
        for (const auto& item : j["gpu_accelerated_tools"]) {
            if (item.is_string()) {
                caps.gpuAcceleratedTools.push_back(item.get<std::string>());
            }
        }
    }
    if (j.contains("capability_score") && j["capability_score"].is_number()) {
        caps.capabilityScore = static_cast<int64_t>(j["capability_score"].get<double>());
    } else {
        caps.capabilityScore = 0;
    }

    if (caps.capabilityScore <= 0) {
        caps.capabilityScore = caps.scoreForTool("");
    }
    return caps;
}

EdaAccelerators::EdaAccelerators(const EdaAcceleratorConfig& config)
    : _config(config),
      _capabilities(probeSystemCapabilities(config.enabled)),
      _hasSimulatedCaps(false) {
}

void EdaAccelerators::setConfig(const EdaAcceleratorConfig& config) {
    _config = config;
    if (!_hasSimulatedCaps) {
        _capabilities = probeSystemCapabilities(_config.enabled);
    }
}

bool EdaAccelerators::isAcceleratorTool(const std::string& toolName) {
    return toolName == "eda_calc_schematic_layout" ||
           toolName == "eda_calc_pcb_placement" ||
           toolName == "eda_calc_pcb_autoroute" ||
           toolName == "eda_calc_pcb_connectivity_audit" ||
           toolName == "eda_calc_schematic_erc_audit" ||
           toolName == "eda_calc_electrical_synth" ||
           toolName == "cad_calc_robot_kinematics_statics" ||
           toolName == "cad_calc_mechanism_synth" ||
           toolName == "cad_calc_print_orientation_dfm";
}

bool EdaAccelerators::isGpuAccelerableTool(const std::string& toolName) {
    return toolName == "cad_calc_print_orientation_dfm" ||
           toolName == "eda_calc_pcb_placement" ||
           toolName == "eda_calc_pcb_autoroute" ||
           toolName == "eda_calc_pcb_connectivity_audit";
}

AcceleratorCapabilities EdaAccelerators::probeSystemCapabilities(bool enabled) {
    return OpenClEngine::instance().getCapabilities(enabled);
}

void EdaAccelerators::setSimulatedCapabilitiesForTest(const AcceleratorCapabilities& caps) {
    _capabilities = caps;
    _hasSimulatedCaps = true;
}

void EdaAccelerators::attachProvenance(nlohmann::json& res, const std::string& toolName) const {
    if (res.is_object() && res.value("ok", false)) {
        if (!res.contains("compute_backend")) {
            res["compute_backend"] = _capabilities.backend;
        }
        if (!res.contains("compute_device")) {
            res["compute_device"] = _capabilities.deviceName;
        }
        if (!res.contains("compute_units")) {
            res["compute_units"] = _capabilities.computeUnits;
        }
        if (!res.contains("capability_score")) {
            res["capability_score"] = _capabilities.scoreForTool(toolName);
        }
        if (!res.contains("gpu_accelerated")) {
            res["gpu_accelerated"] = _capabilities.gpuAvailable && isGpuAccelerableTool(toolName);
        }
    }
}

nlohmann::json EdaAccelerators::executeTool(const std::string& toolName,
                                            const nlohmann::json& args,
                                            bool& outSuccess,
                                            std::string& outSummary) const {
    nlohmann::json res;
    if (toolName == "eda_calc_schematic_layout") {
        res = calcSchematicLayout(args, outSuccess, outSummary);
    } else if (toolName == "eda_calc_pcb_placement") {
        res = calcPcbPlacement(args, outSuccess, outSummary);
    } else if (toolName == "eda_calc_pcb_autoroute") {
        res = calcPcbAutoroute(args, outSuccess, outSummary);
    } else if (toolName == "eda_calc_schematic_erc_audit") {
        res = calcSchematicErcAudit(args, outSuccess, outSummary);
    } else if (toolName == "eda_calc_pcb_connectivity_audit") {
        if (args.is_object() &&
            (args.value("target", "") == "schematic" ||
             (args.contains("wires") && !args.contains("pads") &&
              !args.contains("tracks") && !args.contains("footprints")))) {
            res = calcSchematicErcAudit(args, outSuccess, outSummary);
        } else {
            res = calcPcbConnectivityAudit(args, outSuccess, outSummary);
        }
    } else if (toolName == "eda_calc_electrical_synth") {
        res = calcElectricalSynth(args, outSuccess, outSummary);
    } else if (toolName == "cad_calc_robot_kinematics_statics") {
        res = calcRobotKinematicsStatics(args, outSuccess, outSummary);
    } else if (toolName == "cad_calc_mechanism_synth") {
        res = calcMechanismSynth(args, outSuccess, outSummary);
    } else if (toolName == "cad_calc_print_orientation_dfm") {
        res = calcPrintOrientationDfm(args, outSuccess, outSummary);
    } else {
        outSuccess = false;
        outSummary = "Unknown accelerator tool: " + toolName;
        return {{"ok", false}, {"error", outSummary}};
    }
    attachProvenance(res, toolName);
    return res;
}

nlohmann::json EdaAccelerators::calcSchematicLayout(const nlohmann::json& args,
                                                    bool& outSuccess,
                                                    std::string& outSummary) const {
    if (!args.is_object() || !args.contains("components") || !args["components"].is_array() ||
        args["components"].empty()) {
        outSuccess = false;
        outSummary = "eda_calc_schematic_layout requires non-empty components[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    struct PinSpec {
        std::string number;
        std::string name;
        double dxMm = 0.0;
        double dyMm = 0.0;
    };
    struct CompNode {
        std::string ref;
        std::string category;
        double widthMm = 10.16;
        double heightMm = 10.16;
        int rank = 0;
        double barycenter = 0.0;
        double xMm = 0.0;
        double yMm = 0.0;
        std::vector<PinSpec> pins;
    };

    std::vector<CompNode> nodes;
    std::map<std::string, size_t> refToIdx;
    for (const auto& c : args["components"]) {
        if (!c.is_object()) continue;
        CompNode node;
        node.ref = c.value("reference", c.value("ref", "U" + std::to_string(nodes.size() + 1)));
        node.category = c.value("category", "mcu");
        node.widthMm = snapTo50Mil(std::max(5.08, c.value("width_mm", c.value("width", 10.16))));
        node.heightMm = snapTo50Mil(std::max(5.08, c.value("height_mm", c.value("height", 10.16))));
        node.rank = layerPriorityForCategory(node.category);

        if (c.contains("pins") && c["pins"].is_array()) {
            for (const auto& p : c["pins"]) {
                if (!p.is_object()) continue;
                PinSpec ps;
                ps.number = p.value("number", "1");
                ps.name = p.value("name", ps.number);
                ps.dxMm = snapTo50Mil(p.value("dx_mm", p.value("dx", 0.0)));
                ps.dyMm = snapTo50Mil(p.value("dy_mm", p.value("dy", 0.0)));
                node.pins.push_back(ps);
            }
        }
        if (node.pins.empty()) {
            node.pins.push_back({"1", "IN", -snapTo50Mil(node.widthMm * 0.5), 0.0});
            node.pins.push_back({"2", "OUT", snapTo50Mil(node.widthMm * 0.5), 0.0});
        }
        refToIdx[node.ref] = nodes.size();
        nodes.push_back(node);
    }

    if (nodes.empty()) {
        outSuccess = false;
        outSummary = "No valid component objects in components[]";
        return {{"ok", false}, {"error", outSummary}};
    }

    // Build adjacency from nets for barycentric ordering inside Sugiyama layers
    std::vector<std::vector<size_t>> adj(nodes.size());
    if (args.contains("nets") && args["nets"].is_array()) {
        for (const auto& net : args["nets"]) {
            if (!net.is_object()) continue;
            const nlohmann::json* connArrPtr = nullptr;
            if (net.contains("connections") && net["connections"].is_array()) {
                connArrPtr = &net["connections"];
            } else if (net.contains("pins") && net["pins"].is_array()) {
                connArrPtr = &net["pins"];
            }
            if (connArrPtr == nullptr) continue;
            std::vector<size_t> connectedComps;
            for (const auto& conn : *connArrPtr) {
                if (!conn.is_string()) continue;
                std::string s = conn.get<std::string>();
                size_t dot = s.find('.');
                std::string ref = (dot == std::string::npos) ? s : s.substr(0, dot);
                auto it = refToIdx.find(ref);
                if (it != refToIdx.end()) {
                    connectedComps.push_back(it->second);
                }
            }
            for (size_t i = 0; i < connectedComps.size(); ++i) {
                for (size_t j = i + 1; j < connectedComps.size(); ++j) {
                    adj[connectedComps[i]].push_back(connectedComps[j]);
                    adj[connectedComps[j]].push_back(connectedComps[i]);
                }
            }
        }
    }

    std::map<int, std::vector<size_t>> layers;
    for (size_t i = 0; i < nodes.size(); ++i) {
        layers[nodes[i].rank].push_back(i);
    }

    // Assign initial vertical order and run 2-pass barycenter crossing minimization
    for (auto& kv : layers) {
        for (size_t pos = 0; pos < kv.second.size(); ++pos) {
            nodes[kv.second[pos]].barycenter = static_cast<double>(pos);
        }
    }
    for (int iter = 0; iter < 2; ++iter) {
        for (auto& kv : layers) {
            auto& vec = kv.second;
            for (size_t idx : vec) {
                if (!adj[idx].empty()) {
                    double sum = 0.0;
                    for (size_t nb : adj[idx]) {
                        sum += nodes[nb].barycenter;
                    }
                    nodes[idx].barycenter = sum / static_cast<double>(adj[idx].size());
                }
            }
            std::stable_sort(vec.begin(), vec.end(), [&](size_t a, size_t b) {
                return nodes[a].barycenter < nodes[b].barycenter;
            });
            for (size_t pos = 0; pos < vec.size(); ++pos) {
                nodes[vec[pos]].barycenter = static_cast<double>(pos);
            }
        }
    }

    // Place layers horizontally with 30-grid (38.1 mm) spacing and vertical 20-grid (25.4 mm) spacing
    int colIndex = 0;
    for (const auto& kv : layers) {
        double colX = snapTo50Mil(25.4 + colIndex * 38.1);
        double curY = snapTo50Mil(25.4);
        for (size_t idx : kv.second) {
            nodes[idx].xMm = colX;
            nodes[idx].yMm = curY;
            curY = snapTo50Mil(curY + nodes[idx].heightMm + 15.24);
        }
        ++colIndex;
    }

    // Build pin coordinate lookup and body obstacle set on 50-mil integer grid
    struct GridPt {
        int gx = 0;
        int gy = 0;
        bool operator==(const GridPt& o) const { return gx == o.gx && gy == o.gy; }
        bool operator<(const GridPt& o) const {
            return (gx != o.gx) ? (gx < o.gx) : (gy < o.gy);
        }
    };

    std::set<GridPt> blockedCells;
    std::map<std::string, GridPt> pinGridLookup;

    for (const auto& node : nodes) {
        int cx = toGridCoord(node.xMm, kSchematicGridMm);
        int cy = toGridCoord(node.yMm, kSchematicGridMm);
        int hw = std::max(1, toGridCoord(node.widthMm * 0.5, kSchematicGridMm) - 1);
        int hh = std::max(1, toGridCoord(node.heightMm * 0.5, kSchematicGridMm) - 1);
        for (int gx = cx - hw; gx <= cx + hw; ++gx) {
            for (int gy = cy - hh; gy <= cy + hh; ++gy) {
                blockedCells.insert({gx, gy});
            }
        }
        for (const auto& pin : node.pins) {
            int px = toGridCoord(snapTo50Mil(node.xMm + pin.dxMm), kSchematicGridMm);
            int py = toGridCoord(snapTo50Mil(node.yMm + pin.dyMm), kSchematicGridMm);
            blockedCells.erase({px, py});
            pinGridLookup[node.ref + "." + pin.number] = {px, py};
            pinGridLookup[node.ref + "." + pin.name] = {px, py};
        }
    }

    // Route nets on 50-mil Manhattan grid using A* orthogonal search
    nlohmann::json wiresJson = nlohmann::json::array();
    nlohmann::json labelsJson = nlohmann::json::array();
    int routedNets = 0;

    auto routeTwoPoints = [&](GridPt start, GridPt goal, const std::string& netName) {
        if (start == goal) return;
        int minX = std::min(start.gx, goal.gx) - 15;
        int maxX = std::max(start.gx, goal.gx) + 15;
        int minY = std::min(start.gy, goal.gy) - 15;
        int maxY = std::max(start.gy, goal.gy) + 15;

        struct QNode {
            int f;
            int g;
            GridPt pt;
            int dir;
            bool operator>(const QNode& o) const { return f > o.f; }
        };
        std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> pq;
        std::map<GridPt, int> bestG;
        std::map<GridPt, GridPt> parent;

        auto hCost = [&](GridPt p) {
            return std::abs(p.gx - goal.gx) + std::abs(p.gy - goal.gy);
        };

        pq.push({hCost(start), 0, start, -1});
        bestG[start] = 0;

        const int dx[4] = {1, -1, 0, 0};
        const int dy[4] = {0, 0, 1, -1};
        bool found = false;

        while (!pq.empty()) {
            QNode cur = pq.top();
            pq.pop();
            if (cur.pt == goal) {
                found = true;
                break;
            }
            if (cur.g > bestG[cur.pt]) continue;

            for (int d = 0; d < 4; ++d) {
                GridPt nxt{cur.pt.gx + dx[d], cur.pt.gy + dy[d]};
                if (nxt.gx < minX || nxt.gx > maxX || nxt.gy < minY || nxt.gy > maxY) continue;
                if (!(nxt == goal) && !(nxt == start) && blockedCells.count(nxt)) continue;
                int bendPenalty = (cur.dir != -1 && cur.dir != d) ? 2 : 0;
                int ng = cur.g + 1 + bendPenalty;
                auto it = bestG.find(nxt);
                if (it == bestG.end() || ng < it->second) {
                    bestG[nxt] = ng;
                    parent[nxt] = cur.pt;
                    pq.push({ng + hCost(nxt), ng, nxt, d});
                }
            }
        }

        std::vector<GridPt> path;
        if (found) {
            GridPt cur = goal;
            while (!(cur == start)) {
                path.push_back(cur);
                cur = parent[cur];
            }
            path.push_back(start);
            std::reverse(path.begin(), path.end());
        } else {
            // Fallback L-route snapped to 50-mil grid
            GridPt mid{goal.gx, start.gy};
            path.push_back(start);
            if (!(mid == start) && !(mid == goal)) path.push_back(mid);
            path.push_back(goal);
        }

        // Coalesce collinear grid steps into orthogonal segments
        size_t segStart = 0;
        while (segStart + 1 < path.size()) {
            size_t segEnd = segStart + 1;
            int sdx = path[segEnd].gx - path[segStart].gx;
            int sdy = path[segEnd].gy - path[segStart].gy;
            while (segEnd + 1 < path.size()) {
                int ndx = path[segEnd + 1].gx - path[segEnd].gx;
                int ndy = path[segEnd + 1].gy - path[segEnd].gy;
                if ((sdx == 0 && ndx == 0) || (sdy == 0 && ndy == 0)) {
                    ++segEnd;
                } else {
                    break;
                }
            }
            double x1 = fromGridCoord(path[segStart].gx, kSchematicGridMm);
            double y1 = fromGridCoord(path[segStart].gy, kSchematicGridMm);
            double x2 = fromGridCoord(path[segEnd].gx, kSchematicGridMm);
            double y2 = fromGridCoord(path[segEnd].gy, kSchematicGridMm);
            wiresJson.push_back({
                {"net", netName},
                {"x1_mm", x1},
                {"y1_mm", y1},
                {"x2_mm", x2},
                {"y2_mm", y2}
            });
            segStart = segEnd;
        }
    };

    if (args.contains("nets") && args["nets"].is_array()) {
        for (const auto& net : args["nets"]) {
            if (!net.is_object()) continue;
            const nlohmann::json* connArrPtr = nullptr;
            if (net.contains("connections") && net["connections"].is_array()) {
                connArrPtr = &net["connections"];
            } else if (net.contains("pins") && net["pins"].is_array()) {
                connArrPtr = &net["pins"];
            }
            if (connArrPtr == nullptr) continue;
            std::string netName = net.value("name", "NET");
            std::vector<GridPt> netPins;
            for (const auto& conn : *connArrPtr) {
                if (!conn.is_string()) continue;
                auto it = pinGridLookup.find(conn.get<std::string>());
                if (it != pinGridLookup.end()) {
                    netPins.push_back(it->second);
                }
            }
            if (netPins.size() >= 2) {
                for (size_t i = 0; i + 1 < netPins.size(); ++i) {
                    routeTwoPoints(netPins[i], netPins[i + 1], netName);
                }
                labelsJson.push_back({
                    {"name", netName},
                    {"x_mm", fromGridCoord(netPins[0].gx, kSchematicGridMm)},
                    {"y_mm", fromGridCoord(netPins[0].gy, kSchematicGridMm)}
                });
                ++routedNets;
            }
        }
    }

    nlohmann::json symbolsJson = nlohmann::json::array();
    for (const auto& n : nodes) {
        nlohmann::json pinsArr = nlohmann::json::array();
        for (const auto& p : n.pins) {
            pinsArr.push_back({
                {"number", p.number},
                {"name", p.name},
                {"x_mm", snapTo50Mil(n.xMm + p.dxMm)},
                {"y_mm", snapTo50Mil(n.yMm + p.dyMm)}
            });
        }
        symbolsJson.push_back({
            {"reference", n.ref},
            {"category", n.category},
            {"layer_rank", n.rank},
            {"x_mm", n.xMm},
            {"y_mm", n.yMm},
            {"rotation_deg", 0},
            {"pins", pinsArr}
        });
    }

    outSuccess = true;
    outSummary = "Placed " + std::to_string(nodes.size()) + " symbols • Routed " +
                 std::to_string(routedNets) + " nets on 50-mil grid";
    nlohmann::json res = {
        {"ok", true},
        {"grid_mm", kSchematicGridMm},
        {"symbols", symbolsJson},
        {"wires", wiresJson},
        {"labels", labelsJson},
        {"routed_nets", routedNets},
        {"summary", outSummary}
    };
    attachProvenance(res, "eda_calc_schematic_layout");
    return res;
}

nlohmann::json EdaAccelerators::calcPcbPlacement(const nlohmann::json& args,
                                                 bool& outSuccess,
                                                 std::string& outSummary) const {
    if (!args.is_object() || !args.contains("components") || !args["components"].is_array() ||
        args["components"].empty()) {
        outSuccess = false;
        outSummary = "eda_calc_pcb_placement requires non-empty components[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    double boardW = 60.0;
    double boardH = 40.0;
    double clearanceMm = 0.5;
    if (args.contains("board") && args["board"].is_object()) {
        boardW = std::max(10.0, args["board"].value("width_mm", args["board"].value("width", 60.0)));
        boardH = std::max(10.0, args["board"].value("height_mm", args["board"].value("height", 40.0)));
        clearanceMm = std::max(0.1, args["board"].value("clearance_mm", args["board"].value("clearance", 0.5)));
    }

    struct PcbItem {
        std::string ref;
        double w = 3.0;
        double h = 3.0;
        double x = 0.0;
        double y = 0.0;
        int rotDeg = 0;
        std::string layer = "F.Cu";
        bool fixed = false;
        std::string decouplesRef;
    };

    std::vector<PcbItem> items;
    std::map<std::string, size_t> refIdx;
    int cols = std::max(1, static_cast<int>(std::ceil(std::sqrt(static_cast<double>(args["components"].size())))));
    for (size_t i = 0; i < args["components"].size(); ++i) {
        const auto& c = args["components"][i];
        if (!c.is_object()) continue;
        PcbItem item;
        item.ref = c.value("reference", c.value("ref", "U" + std::to_string(i + 1)));
        item.w = std::max(0.5, c.value("width_mm", c.value("width", 3.0)));
        item.h = std::max(0.5, c.value("height_mm", c.value("height", 3.0)));
        item.fixed = c.value("fixed", false);
        item.layer = c.value("layer", "F.Cu");
        item.rotDeg = c.value("rotation_deg", c.value("rotation", 0));
        item.decouplesRef = c.value("decouples", "");

        int r = static_cast<int>(i) / cols;
        int col = static_cast<int>(i) % cols;
        double initX = (col + 1) * (boardW / (cols + 1));
        double initY = (r + 1) * (boardH / (cols + 1));
        item.x = c.value("x_mm", c.value("x", initX));
        item.y = c.value("y_mm", c.value("y", initY));
        refIdx[item.ref] = items.size();
        items.push_back(item);
    }

    std::vector<std::vector<size_t>> netLists;
    if (args.contains("nets") && args["nets"].is_array()) {
        for (const auto& net : args["nets"]) {
            if (!net.is_object()) continue;
            const nlohmann::json* pinArrPtr = nullptr;
            if (net.contains("pins") && net["pins"].is_array()) {
                pinArrPtr = &net["pins"];
            } else if (net.contains("connections") && net["connections"].is_array()) {
                pinArrPtr = &net["connections"];
            }
            if (pinArrPtr == nullptr) continue;
            std::vector<size_t> members;
            for (const auto& p : *pinArrPtr) {
                if (!p.is_string()) continue;
                std::string s = p.get<std::string>();
                size_t dot = s.find('.');
                std::string ref = (dot == std::string::npos) ? s : s.substr(0, dot);
                auto it = refIdx.find(ref);
                if (it != refIdx.end()) members.push_back(it->second);
            }
            if (members.size() >= 2) netLists.push_back(members);
        }
    }

    auto effW = [](const PcbItem& it) { return (it.rotDeg % 180 != 0) ? it.h : it.w; };
    auto effH = [](const PcbItem& it) { return (it.rotDeg % 180 != 0) ? it.w : it.h; };

    auto computeCost = [&](const std::vector<PcbItem>& state, double& outHpwl, int& outOverlaps) {
        outHpwl = 0.0;
        for (const auto& nl : netLists) {
            double minX = state[nl[0]].x, maxX = state[nl[0]].x;
            double minY = state[nl[0]].y, maxY = state[nl[0]].y;
            for (size_t idx : nl) {
                minX = std::min(minX, state[idx].x);
                maxX = std::max(maxX, state[idx].x);
                minY = std::min(minY, state[idx].y);
                maxY = std::max(maxY, state[idx].y);
            }
            outHpwl += (maxX - minX) + (maxY - minY);
        }

        double overlapArea = 0.0;
        outOverlaps = 0;
        for (size_t i = 0; i < state.size(); ++i) {
            for (size_t j = i + 1; j < state.size(); ++j) {
                if (state[i].layer != state[j].layer) continue;
                double wi = effW(state[i]) * 0.5 + clearanceMm;
                double hi = effH(state[i]) * 0.5 + clearanceMm;
                double wj = effW(state[j]) * 0.5 + clearanceMm;
                double hj = effH(state[j]) * 0.5 + clearanceMm;
                double ox = (wi + wj) - std::fabs(state[i].x - state[j].x);
                double oy = (hi + hj) - std::fabs(state[i].y - state[j].y);
                if (ox > 0.0 && oy > 0.0) {
                    overlapArea += ox * oy;
                    ++outOverlaps;
                }
            }
        }

        double decapPenalty = 0.0;
        for (const auto& it : state) {
            if (!it.decouplesRef.empty()) {
                auto targetIt = refIdx.find(it.decouplesRef);
                if (targetIt != refIdx.end()) {
                    const auto& target = state[targetIt->second];
                    double dist = std::hypot(it.x - target.x, it.y - target.y);
                    double idealDist = (effW(target) * 0.5 + effW(it) * 0.5 + clearanceMm * 2.0 + 0.5);
                    if (dist > idealDist + 2.5) {
                        decapPenalty += (dist - idealDist) * 25.0;
                    }
                }
            }
        }

        return outHpwl + overlapArea * 500.0 + decapPenalty;
    };

    // Deterministic xorshift64* PRNG for Simulated Annealing
    uint64_t rngState = 0x9E3779B97F4A7C15ULL;
    auto nextRand = [&]() -> double {
        rngState ^= rngState >> 12;
        rngState ^= rngState << 25;
        rngState ^= rngState >> 27;
        uint64_t val = rngState * 2685821657736338717ULL;
        return static_cast<double>(val & 0xFFFFFFULL) / static_cast<double>(0x1000000ULL);
    };

    double curHpwl = 0.0;
    int curOverlaps = 0;
    double curCost = computeCost(items, curHpwl, curOverlaps);
    std::vector<PcbItem> bestItems = items;
    double bestCost = curCost;

    bool gpuKernelExecuted = false;
    if (_capabilities.gpuAvailable && OpenClEngine::instance().isGpuAvailable() && items.size() >= 2) {
        int numReplicas = 64;
        std::vector<float> itemDims;
        for (const auto& it : items) {
            itemDims.push_back(static_cast<float>(it.w));
            itemDims.push_back(static_cast<float>(it.h));
            itemDims.push_back(static_cast<float>(it.rotDeg));
            itemDims.push_back(it.layer == "F.Cu" ? 0.0f : 1.0f);
        }
        std::vector<int> netOffsets;
        std::vector<int> netMembers;
        netOffsets.push_back(0);
        for (const auto& nl : netLists) {
            for (size_t m : nl) {
                netMembers.push_back(static_cast<int>(m));
            }
            netOffsets.push_back(static_cast<int>(netMembers.size()));
        }

        std::vector<float> replicaCoords;
        replicaCoords.reserve(static_cast<size_t>(numReplicas * items.size() * 2));
        for (int r = 0; r < numReplicas; ++r) {
            for (size_t i = 0; i < items.size(); ++i) {
                double rx = items[i].x;
                double ry = items[i].y;
                if (!items[i].fixed && r > 0) {
                    rx += (nextRand() * 2.0 - 1.0) * (boardW * 0.2);
                    ry += (nextRand() * 2.0 - 1.0) * (boardH * 0.2);
                    double hw = effW(items[i]) * 0.5 + clearanceMm;
                    double hh = effH(items[i]) * 0.5 + clearanceMm;
                    rx = std::clamp(rx, hw, std::max(hw, boardW - hw));
                    ry = std::clamp(ry, hh, std::max(hh, boardH - hh));
                }
                replicaCoords.push_back(static_cast<float>(rx));
                replicaCoords.push_back(static_cast<float>(ry));
            }
        }

        std::vector<float> outCosts;
        if (OpenClEngine::instance().runPlacementWirelength(
                numReplicas, static_cast<int>(items.size()), static_cast<int>(netLists.size()),
                static_cast<float>(clearanceMm), itemDims, replicaCoords, netOffsets, netMembers, outCosts)) {
            gpuKernelExecuted = true;
            int bestRep = 0;
            float bestRepCost = outCosts[0];
            for (int r = 1; r < numReplicas; ++r) {
                if (outCosts[r * 2 + 0] < bestRepCost) {
                    bestRepCost = outCosts[r * 2 + 0];
                    bestRep = r;
                }
            }
            for (size_t i = 0; i < items.size(); ++i) {
                items[i].x = replicaCoords[(bestRep * items.size() + i) * 2 + 0];
                items[i].y = replicaCoords[(bestRep * items.size() + i) * 2 + 1];
            }
            bestItems = items;
        }
    }

    if (!gpuKernelExecuted) {
        int totalIters = std::clamp(_config.saPlacementIterations, 500, 15000);
        for (int iter = 0; iter < totalIters; ++iter) {
            double temp = std::max(0.05, 10.0 * (1.0 - static_cast<double>(iter) / totalIters));
            size_t idx = static_cast<size_t>(nextRand() * items.size()) % items.size();
            if (items[idx].fixed) continue;

            PcbItem saved = items[idx];
            double moveRange = std::max(0.5, boardW * 0.25 * (temp / 10.0));
            items[idx].x += (nextRand() * 2.0 - 1.0) * moveRange;
            items[idx].y += (nextRand() * 2.0 - 1.0) * moveRange;
            if (nextRand() < 0.15) {
                items[idx].rotDeg = (items[idx].rotDeg + 90) % 360;
            }

            double halfW = effW(items[idx]) * 0.5 + clearanceMm;
            double halfH = effH(items[idx]) * 0.5 + clearanceMm;
            items[idx].x = std::clamp(items[idx].x, halfW, std::max(halfW, boardW - halfW));
            items[idx].y = std::clamp(items[idx].y, halfH, std::max(halfH, boardH - halfH));

            double newHpwl = 0.0;
            int newOverlaps = 0;
            double newCost = computeCost(items, newHpwl, newOverlaps);
            double delta = newCost - curCost;
            if (delta <= 0.0 || std::exp(-delta / temp) > nextRand()) {
                curCost = newCost;
                if (newCost < bestCost) {
                    bestCost = newCost;
                    bestItems = items;
                }
            } else {
                items[idx] = saved;
            }
        }
    }

    items = bestItems;

    // Final deterministic overlap legalizer pass
    for (int pass = 0; pass < 40; ++pass) {
        double hpwl = 0.0;
        int overlaps = 0;
        computeCost(items, hpwl, overlaps);
        if (overlaps == 0) break;
        for (size_t i = 0; i < items.size(); ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                if (items[i].layer != items[j].layer) continue;
                double wi = effW(items[i]) * 0.5 + clearanceMm;
                double hi = effH(items[i]) * 0.5 + clearanceMm;
                double wj = effW(items[j]) * 0.5 + clearanceMm;
                double hj = effH(items[j]) * 0.5 + clearanceMm;
                double ox = (wi + wj) - std::fabs(items[i].x - items[j].x);
                double oy = (hi + hj) - std::fabs(items[i].y - items[j].y);
                if (ox > 0.0 && oy > 0.0) {
                    size_t mover = items[j].fixed ? i : j;
                    if (!items[mover].fixed) {
                        if (ox < oy) {
                            items[mover].x += (items[mover].x >= items[i].x) ? (ox + 0.25) : -(ox + 0.25);
                        } else {
                            items[mover].y += (items[mover].y >= items[i].y) ? (oy + 0.25) : -(oy + 0.25);
                        }
                        double hw = effW(items[mover]) * 0.5 + clearanceMm;
                        double hh = effH(items[mover]) * 0.5 + clearanceMm;
                        items[mover].x = std::clamp(items[mover].x, hw, std::max(hw, boardW - hw));
                        items[mover].y = std::clamp(items[mover].y, hh, std::max(hh, boardH - hh));
                    }
                }
            }
        }
    }

    double finalHpwl = 0.0;
    int finalOverlaps = 0;
    computeCost(items, finalHpwl, finalOverlaps);

    nlohmann::json placementsJson = nlohmann::json::array();
    for (const auto& it : items) {
        placementsJson.push_back({
            {"reference", it.ref},
            {"x_mm", std::round(it.x * 100.0) / 100.0},
            {"y_mm", std::round(it.y * 100.0) / 100.0},
            {"rotation_deg", it.rotDeg},
            {"layer", it.layer}
        });
    }

    outSuccess = (finalOverlaps == 0);
    outSummary = "Placed " + std::to_string(items.size()) + " footprints • HPWL " +
                 std::to_string(static_cast<int>(std::round(finalHpwl))) + " mm • " +
                 std::to_string(finalOverlaps) + " overlaps";
    nlohmann::json res = {
        {"ok", outSuccess},
        {"placements", placementsJson},
        {"hpwl_mm", std::round(finalHpwl * 100.0) / 100.0},
        {"courtyard_overlaps", finalOverlaps},
        {"summary", outSummary}
    };
    if (gpuKernelExecuted) {
        res["gpu_kernel_executed"] = true;
    }
    attachProvenance(res, "eda_calc_pcb_placement");
    return res;
}

nlohmann::json EdaAccelerators::calcPcbAutoroute(const nlohmann::json& args,
                                                 bool& outSuccess,
                                                 std::string& outSummary) const {
    auto fail = [&outSuccess, &outSummary](const std::string& msg) -> nlohmann::json {
        outSuccess = false;
        outSummary = msg;
        return {{"ok", false}, {"error", msg}};
    };
    if (!args.is_object() || !args.contains("nets") || !args["nets"].is_array()) {
        return fail("eda_calc_pcb_autoroute requires nets[] array");
    }

    // ---- 1. Coordinate units --------------------------------------------------------------
    // *_mm keys are always millimetres. Bare keys (x, y, x1, width, diameter, drill, size_x, ...)
    // are read in args.units ("mm" default, or "mil"). There is no magnitude-based guessing.
    std::string units = "mm";
    if (args.contains("units")) {
        if (!args["units"].is_string()) {
            return fail("eda_calc_pcb_autoroute: units must be \"mm\" or \"mil\"");
        }
        units = args["units"].get<std::string>();
        std::transform(units.begin(), units.end(), units.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    }
    if (units != "mm" && units != "mil") {
        return fail("eda_calc_pcb_autoroute: units must be \"mm\" or \"mil\" (got \"" + units + "\")");
    }
    const double unitScale = (units == "mil") ? kMmPerMil : 1.0;

    auto readNum = [](const nlohmann::json& o, std::initializer_list<const char*> keys,
                      double defVal) -> double {
        if (!o.is_object()) return defVal;
        for (const char* k : keys) {
            auto it = o.find(k);
            if (it != o.end() && it->is_number()) return it->get<double>();
        }
        return defVal;
    };
    auto readLen = [unitScale](const nlohmann::json& o,
                               std::initializer_list<const char*> mmKeys,
                               std::initializer_list<const char*> bareKeys,
                               double defMm) -> double {
        if (!o.is_object()) return defMm;
        for (const char* k : mmKeys) {
            auto it = o.find(k);
            if (it != o.end() && it->is_number()) return it->get<double>();
        }
        for (const char* k : bareKeys) {
            auto it = o.find(k);
            if (it != o.end() && it->is_number()) return it->get<double>() * unitScale;
        }
        return defMm;
    };
    auto netNameOf = [](const nlohmann::json& o, const std::string& defVal) -> std::string {
        if (o.is_object() && o.contains("net") && o["net"].is_string()) {
            return o["net"].get<std::string>();
        }
        return defVal;
    };

    // ---- 2. Board, layer stack and design rules -------------------------------------------
    double boardW = 40.0;
    double boardH = 30.0;
    int numLayers = 2;
    bool layersExplicit = false;
    bool useEasyEdaNames = false;
    bool boardOriginGiven = false;
    double boardOriginX = 0.0;
    double boardOriginY = 0.0;
    if (args.contains("board") && args["board"].is_object()) {
        const auto& b = args["board"];
        boardW = std::max(5.0, readNum(b, {"width_mm"}, 40.0));
        boardH = std::max(5.0, readNum(b, {"height_mm"}, 30.0));
        if (b.contains("layers") && b["layers"].is_number()) {
            numLayers = std::clamp(static_cast<int>(b["layers"].get<double>()), 1, 6);
            layersExplicit = true;
        }
        if (b.contains("layer_style") && b["layer_style"].is_string()) {
            std::string style = b["layer_style"].get<std::string>();
            if (style == "easyeda" || style == "EasyEDA") {
                useEasyEdaNames = true;
            }
        }
        if (b.contains("origin_x_mm") && b["origin_x_mm"].is_number() &&
            b.contains("origin_y_mm") && b["origin_y_mm"].is_number()) {
            boardOriginGiven = true;
            boardOriginX = b["origin_x_mm"].get<double>();
            boardOriginY = b["origin_y_mm"].get<double>();
        }
    }

    auto isEasyEdaLayerToken = [](const std::string& l) -> bool {
        return l == "TopLayer" || l == "BottomLayer" ||
               l == "InnerLayer1" || l == "InnerLayer2" ||
               l == "InnerLayer3" || l == "InnerLayer4" ||
               l == "MultiLayer" || l == "1" || l == "2" ||
               l == "12" || l == "15" || l == "16" || l == "17" || l == "18";
    };

    auto extractLayerStr = [](const nlohmann::json& obj, const std::string& defVal) -> std::string {
        if (!obj.is_object() || !obj.contains("layer")) return defVal;
        if (obj["layer"].is_string()) return obj["layer"].get<std::string>();
        if (obj["layer"].is_number_integer()) {
            int v = obj["layer"].get<int>();
            if (v == 12 || v < 0) return "MultiLayer";
            return "__IDX_" + std::to_string(v);
        }
        return defVal;
    };

    // Detect layer naming style and the deepest inner layer referenced by any primitive.
    bool useEasyEdaNumeric = false;
    int innerLayersReferenced = 0;
    auto scanLayerOf = [&](const nlohmann::json& o) {
        if (!o.is_object()) return;
        std::string l = extractLayerStr(o, "");
        if (l.empty()) return;
        if (isEasyEdaLayerToken(l)) useEasyEdaNames = true;
        if (l == "1" || l == "2" || l == "15" || l == "16" || l == "17" || l == "18") {
            useEasyEdaNumeric = true;
        }
        int inner = 0;
        if (l == "15" || l == "InnerLayer1" || l == "In1.Cu") inner = 1;
        else if (l == "16" || l == "InnerLayer2" || l == "In2.Cu") inner = 2;
        else if (l == "17" || l == "InnerLayer3" || l == "In3.Cu") inner = 3;
        else if (l == "18" || l == "InnerLayer4" || l == "In4.Cu") inner = 4;
        innerLayersReferenced = std::max(innerLayersReferenced, inner);
    };
    for (const auto& n : args["nets"]) {
        if (!n.is_object() || !n.contains("pads") || !n["pads"].is_array()) continue;
        for (const auto& p : n["pads"]) scanLayerOf(p);
    }
    for (const char* key : {"all_pads", "pads", "existing_tracks", "obstacles", "keepouts", "pours", "zones"}) {
        if (!args.contains(key) || !args[key].is_array()) continue;
        for (const auto& o : args[key]) scanLayerOf(o);
    }

    std::vector<std::string> warnings;
    // Fabricated copper stackups are even (2/4/6): any inner-layer reference implies at least 4 layers.
    int layersRequired = (innerLayersReferenced > 0) ? innerLayersReferenced + 2 : 0;
    if (layersRequired % 2 != 0) ++layersRequired;
    if (layersRequired > numLayers) {
        if (layersExplicit) {
            warnings.push_back("board.layers=" + std::to_string(numLayers) +
                               " but primitives reference inner layer " +
                               std::to_string(innerLayersReferenced) + "; routing grid expanded to " +
                               std::to_string(std::min(6, layersRequired)) + " layers");
        }
        numLayers = std::min(6, layersRequired);
    }

    double trackWidthMm = std::max(0.08, args.value("track_width_mm", 0.25));
    double clearanceMm = std::max(0.08, args.value("clearance_mm", 0.20));
    double viaSizeMm = std::max(0.25, args.value("via_size_mm", args.value("via_diameter_mm", 0.60)));
    double viaDrillMm = std::max(0.12, args.value("via_drill_mm", 0.30));
    double gridMm = std::clamp(args.value("grid_step_mm", _config.autorouteGridMm), 0.10, 1.0);
    int maxPasses = std::clamp(_config.autorouteMaxRipupPasses, 1, 50);
    if (args.contains("max_ripup_passes") && args["max_ripup_passes"].is_number_integer()) {
        maxPasses = std::clamp(args["max_ripup_passes"].get<int>(), 1, 50);
    }
    if (args.contains("rules") && args["rules"].is_object()) {
        const auto& r = args["rules"];
        trackWidthMm = std::max(0.08, r.value("track_width_mm", trackWidthMm));
        clearanceMm = std::max(0.08, r.value("clearance_mm", clearanceMm));
        viaSizeMm = std::max(0.25, r.value("via_size_mm", r.value("via_diameter_mm", viaSizeMm)));
        viaDrillMm = std::max(0.12, r.value("via_drill_mm", viaDrillMm));
        if (r.contains("grid_step_mm") && r["grid_step_mm"].is_number()) {
            gridMm = std::clamp(r["grid_step_mm"].get<double>(), 0.10, 1.0);
        }
        if (r.contains("max_ripup_passes") && r["max_ripup_passes"].is_number_integer()) {
            maxPasses = std::clamp(r["max_ripup_passes"].get<int>(), 1, 50);
        }
    }
    const double requestedGridMm = gridMm;

    // Returns -1 for MultiLayer / PTH pads that span all copper layers
    auto layerNameToIdx = [&](const std::string& l) -> int {
        if (l.rfind("__IDX_", 0) == 0) {
            int idx = std::atoi(l.c_str() + 6);
            return std::clamp(idx, 0, numLayers - 1);
        }
        if (l == "MultiLayer" || l == "12" || l == "*.Cu" ||
            l == "Multi-Layer" || l == "PTH" || l == "thru_hole") {
            return -1;
        }
        if ((l == "B.Cu" || l == "BottomLayer" || l == "2" || l == "Bottom") && numLayers >= 2) {
            return numLayers - 1;
        }
        if ((l == "In1.Cu" || l == "InnerLayer1" || l == "15") && numLayers >= 3) return 1;
        if ((l == "In2.Cu" || l == "InnerLayer2" || l == "16") && numLayers >= 4) return 2;
        if ((l == "In3.Cu" || l == "InnerLayer3" || l == "17") && numLayers >= 5) return 3;
        if ((l == "In4.Cu" || l == "InnerLayer4" || l == "18") && numLayers >= 6) return 4;
        return 0;
    };
    auto layerIdxToName = [&](int idx) -> std::string {
        if (useEasyEdaNumeric) {
            if (idx <= 0) return "1";
            if (idx == numLayers - 1 && numLayers >= 2) return "2";
            return std::to_string(14 + idx);
        }
        if (useEasyEdaNames) {
            if (idx <= 0) return "TopLayer";
            if (idx == numLayers - 1 && numLayers >= 2) return "BottomLayer";
            return "InnerLayer" + std::to_string(idx);
        }
        if (idx <= 0) return "F.Cu";
        if (idx == numLayers - 1 && numLayers >= 2) return "B.Cu";
        return "In" + std::to_string(idx) + ".Cu";
    };

    // ---- 3. Parse every primitive once into millimetre geometry ---------------------------
    std::map<std::string, int32_t> netIdByName;
    auto netIdFor = [&netIdByName](const std::string& name) -> int32_t {
        if (name.empty() || name == "__FOREIGN_PAD__") return kCellBlockAll;
        auto it = netIdByName.find(name);
        if (it != netIdByName.end()) return it->second;
        int32_t id = static_cast<int32_t>(netIdByName.size()) + 1;
        netIdByName.emplace(name, id);
        return id;
    };

    struct PadGeom {
        double x = 0.0;
        double y = 0.0;
        double hw = 0.0;
        double hh = 0.0;
        double cosR = 1.0;
        double sinR = 0.0;
        int layer = 0; // -1 = all copper layers
        int32_t netId = kCellBlockAll;
    };
    struct TrackGeom {
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        double w = 0.0;
        int layer = 0;
        int32_t netId = kCellBlockAll;
    };
    struct ViaGeom {
        double x = 0.0;
        double y = 0.0;
        double d = 0.0;
        int32_t netId = kCellBlockAll;
    };
    struct RectGeom {
        double cx = 0.0;
        double cy = 0.0;
        double hw = 0.0;
        double hh = 0.0;
        int layer = -1;
        int32_t ownerId = kCellBlockAll;
        double margin = 0.0;
        double viaMargin = -1.0; // < 0: does not block vias
    };

    auto extractPolySpan = [](const nlohmann::json& poly, double scale, double& outW, double& outH) {
        if (!poly.is_array() || poly.empty()) return;
        double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
        int count = 0;
        if (poly[0].is_number()) {
            for (size_t i = 0; i + 1 < poly.size(); i += 2) {
                if (!poly[i].is_number() || !poly[i + 1].is_number()) continue;
                double px = poly[i].get<double>();
                double py = poly[i + 1].get<double>();
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
                ++count;
            }
        } else {
            for (const auto& pt : poly) {
                if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                    double px = pt[0].get<double>();
                    double py = pt[1].get<double>();
                    minX = std::min(minX, px);
                    maxX = std::max(maxX, px);
                    minY = std::min(minY, py);
                    maxY = std::max(maxY, py);
                    ++count;
                }
            }
        }
        if (count >= 2) {
            if (outW <= 0.0 && maxX > minX) outW = (maxX - minX) * scale;
            if (outH <= 0.0 && maxY > minY) outH = (maxY - minY) * scale;
        }
    };

    auto parsePad = [&](const nlohmann::json& p, int32_t netId) -> PadGeom {
        PadGeom g;
        g.x = readLen(p, {"x_mm"}, {"x"}, 0.0);
        g.y = readLen(p, {"y_mm"}, {"y"}, 0.0);
        double w = readLen(p, {"w_mm", "width_mm"}, {"size_x", "width"}, 0.0);
        double h = readLen(p, {"h_mm", "height_mm"}, {"size_y", "height"}, 0.0);
        if ((w <= 0.0 || h <= 0.0) && p.contains("pad") && p["pad"].is_array() && !p["pad"].empty() &&
            p["pad"][0].is_string()) {
            std::string shape = p["pad"][0].get<std::string>();
            for (char& c : shape) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if ((shape == "OVAL" || shape == "RECT" || shape == "ELLIPSE" || shape == "OCTAGON") &&
                p["pad"].size() >= 3 && p["pad"][1].is_number() && p["pad"][2].is_number()) {
                if (w <= 0.0) w = p["pad"][1].get<double>() * unitScale;
                if (h <= 0.0) h = p["pad"][2].get<double>() * unitScale;
            } else if ((shape == "ROUND" || shape == "CIRCLE") &&
                       p["pad"].size() >= 2 && p["pad"][1].is_number()) {
                double d = p["pad"][1].get<double>() * unitScale;
                if (w <= 0.0) w = d;
                if (h <= 0.0) h = d;
            } else if (shape == "POLYGON" && p["pad"].size() >= 2) {
                extractPolySpan(p["pad"][1], unitScale, w, h);
            }
        }
        if ((w <= 0.0 || h <= 0.0) && p.contains("polygon") && p["polygon"].is_array()) {
            extractPolySpan(p["polygon"], unitScale, w, h);
        }
        double holeMm = readLen(p, {"drill_mm"}, {"drill", "hole_diameter"}, 0.0);
        if (holeMm <= 0.0 && p.contains("hole") && p["hole"].is_array() &&
            p["hole"].size() >= 2 && p["hole"][1].is_number()) {
            holeMm = p["hole"][1].get<double>() * unitScale;
            if (p["hole"].size() >= 3 && p["hole"][2].is_number()) {
                holeMm = std::max(holeMm, p["hole"][2].get<double>() * unitScale);
            }
        }
        if (holeMm > 0.0) {
            w = std::max(w, holeMm);
            h = std::max(h, holeMm);
        }
        if (w <= 0.0) w = trackWidthMm;
        if (h <= 0.0) h = trackWidthMm;
        g.hw = 0.5 * w;
        g.hh = 0.5 * h;
        double rad = readNum(p, {"rotation_deg", "rotation"}, 0.0) * kPi / 180.0;
        g.cosR = std::cos(rad);
        g.sinR = std::sin(rad);
        g.layer = layerNameToIdx(extractLayerStr(p, "F.Cu"));
        if (holeMm > 0.0) {
            g.layer = -1;
        }
        g.netId = netId;
        return g;
    };

    std::vector<PadGeom> staticPads;
    std::vector<TrackGeom> tracksIn;
    std::vector<ViaGeom> viasIn;
    std::vector<RectGeom> rects;

    struct CopperCell {
        size_t cid = 0;
        int island = 0;
        double ex = 0.0;
        double ey = 0.0;
        bool isInterior = false;
    };
    struct Bridge {
        std::vector<size_t> cells;
        double sx = 0.0;
        double sy = 0.0;
        double ex = 0.0;
        double ey = 0.0;
    };
    struct NetSpec {
        std::string name;
        int32_t id = 0;
        double widthMm = 0.25;
        std::vector<PadGeom> pads;
        std::vector<CopperCell> copper; // seed/goal cells of pad-bearing islands
        int islandCount = 0;
        int startIsland = 0;
        int requiredIslands = 0;
        int padlessIslands = 0;
        std::vector<Bridge> bridges;
        std::vector<size_t> centerCells;
        std::vector<size_t> viaSites2D;
        std::vector<size_t> haloCells;
        std::vector<size_t> viaHalo2D;
        std::vector<size_t> newVia2D;
        bool routed = false;
    };

    std::vector<NetSpec> nets;
    for (const auto& n : args["nets"]) {
        if (!n.is_object() || !n.contains("pads") || !n["pads"].is_array()) continue;
        NetSpec ns;
        if (n.contains("name") && n["name"].is_string() && !n["name"].get<std::string>().empty()) {
            ns.name = n["name"].get<std::string>();
        } else {
            ns.name = "NET_" + std::to_string(nets.size() + 1);
        }
        ns.id = netIdFor(ns.name);
        ns.widthMm = std::max(0.08, readNum(n, {"width_mm", "track_width_mm"}, trackWidthMm));
        for (const auto& p : n["pads"]) {
            if (!p.is_object()) continue;
            PadGeom g = parsePad(p, ns.id);
            staticPads.push_back(g);
            ns.pads.push_back(g);
        }
        if (ns.pads.size() >= 2) nets.push_back(std::move(ns));
    }
    for (const char* padArrKey : {"all_pads", "pads"}) {
        if (!args.contains(padArrKey) || !args[padArrKey].is_array()) continue;
        for (const auto& p : args[padArrKey]) {
            if (!p.is_object()) continue;
            staticPads.push_back(parsePad(p, netIdFor(netNameOf(p, ""))));
        }
    }
    const nlohmann::json* tracksArr = args.contains("existing_tracks") && args["existing_tracks"].is_array()
        ? &args["existing_tracks"]
        : (args.contains("tracks") && args["tracks"].is_array() ? &args["tracks"] : nullptr);
    if (tracksArr) {
        for (const auto& et : *tracksArr) {
            if (!et.is_object()) continue;
            TrackGeom t;
            t.x1 = readLen(et, {"x1_mm"}, {"x1"}, 0.0);
            t.y1 = readLen(et, {"y1_mm"}, {"y1"}, 0.0);
            t.x2 = readLen(et, {"x2_mm"}, {"x2"}, 0.0);
            t.y2 = readLen(et, {"y2_mm"}, {"y2"}, 0.0);
            t.w = std::max(0.0, readLen(et, {"width_mm"}, {"width"}, trackWidthMm));
            t.layer = std::max(0, layerNameToIdx(extractLayerStr(et, "F.Cu")));
            t.netId = netIdFor(netNameOf(et, ""));
            tracksIn.push_back(t);
        }
    }
    const nlohmann::json* viasArr = args.contains("existing_vias") && args["existing_vias"].is_array()
        ? &args["existing_vias"]
        : (args.contains("vias") && args["vias"].is_array() ? &args["vias"] : nullptr);
    if (viasArr) {
        for (const auto& ev : *viasArr) {
            if (!ev.is_object()) continue;
            ViaGeom v;
            v.x = readLen(ev, {"x_mm"}, {"x"}, 0.0);
            v.y = readLen(ev, {"y_mm"}, {"y"}, 0.0);
            v.d = std::max(0.0, readLen(ev, {"size_mm", "diameter_mm"}, {"diameter"}, viaSizeMm));
            v.netId = netIdFor(netNameOf(ev, ""));
            viasIn.push_back(v);
        }
    }
    if (args.contains("obstacles") && args["obstacles"].is_array()) {
        for (const auto& obs : args["obstacles"]) {
            if (!obs.is_object()) continue;
            RectGeom r;
            r.cx = readLen(obs, {"x_mm"}, {"x"}, 0.0);
            r.cy = readLen(obs, {"y_mm"}, {"y"}, 0.0);
            r.hw = 0.5 * readLen(obs, {"width_mm", "w_mm"}, {"size_x"}, 1.0);
            r.hh = 0.5 * readLen(obs, {"height_mm", "h_mm"}, {"size_y"}, 1.0);
            r.layer = layerNameToIdx(extractLayerStr(obs, "F.Cu"));
            r.ownerId = kCellBlockAll;
            r.margin = clearanceMm + 0.5 * trackWidthMm;
            r.viaMargin = clearanceMm + 0.5 * viaSizeMm;
            rects.push_back(r);
        }
    }
    // keepouts[] support corner+w_mm/h_mm, x_min/x_max, or center+width_mm/height_mm
    if (args.contains("keepouts") && args["keepouts"].is_array()) {
        for (const auto& ko : args["keepouts"]) {
            if (!ko.is_object()) continue;
            double xMin = 0.0, xMax = 0.0, yMin = 0.0, yMax = 0.0;
            if (ko.contains("x_min") && ko.contains("x_max")) {
                xMin = readLen(ko, {}, {"x_min"}, 0.0);
                xMax = readLen(ko, {}, {"x_max"}, 0.0);
                yMin = readLen(ko, {}, {"y_min"}, 0.0);
                yMax = readLen(ko, {}, {"y_max"}, 0.0);
            } else if (ko.contains("w_mm") || ko.contains("h_mm")) {
                double kx = readLen(ko, {"x_mm"}, {"x"}, 0.0);
                double ky = readLen(ko, {"y_mm"}, {"y"}, 0.0);
                double kw = readLen(ko, {"w_mm", "width_mm"}, {}, 1.0);
                double kh = readLen(ko, {"h_mm", "height_mm"}, {}, 1.0);
                xMin = kx;
                xMax = kx + kw;
                yMin = ky;
                yMax = ky + kh;
            } else {
                double kx = readLen(ko, {"x_mm"}, {"x"}, 0.0);
                double ky = readLen(ko, {"y_mm"}, {"y"}, 0.0);
                double kw = readLen(ko, {"width_mm"}, {"size_x"}, 1.0);
                double kh = readLen(ko, {"height_mm"}, {"size_y"}, 1.0);
                xMin = kx - 0.5 * kw;
                xMax = kx + 0.5 * kw;
                yMin = ky - 0.5 * kh;
                yMax = ky + 0.5 * kh;
            }
            if (xMax < xMin || yMax < yMin) continue;
            RectGeom r;
            r.cx = 0.5 * (xMin + xMax);
            r.cy = 0.5 * (yMin + yMax);
            r.hw = 0.5 * (xMax - xMin);
            r.hh = 0.5 * (yMax - yMin);
            r.layer = layerNameToIdx(extractLayerStr(ko, "MultiLayer"));
            r.ownerId = kCellBlockAll;
            r.margin = clearanceMm + 0.5 * trackWidthMm;
            r.viaMargin = clearanceMm + 0.5 * viaSizeMm;
            rects.push_back(r);
        }
    }
    // pours[] / zones[] / copper_pours[] are net-owned copper areas on their layer.
    struct RoutePour {
        std::string id;
        std::string net;
        int layer = 0;
        std::vector<std::pair<double, double>> poly;
        bool hasBbox = false;
        double xMin = 0.0;
        double yMin = 0.0;
        double xMax = 0.0;
        double yMax = 0.0;
    };
    std::vector<RoutePour> allRoutePours;

    for (const char* pourKey : {"pours", "zones", "copper_pours"}) {
        if (!args.contains(pourKey) || !args[pourKey].is_array()) continue;
        for (const auto& z : args[pourKey]) {
            if (!z.is_object()) continue;
            RoutePour rp;
            rp.id = z.value("id", "pour_" + std::to_string(allRoutePours.size() + 1));
            rp.net = netNameOf(z, "");
            rp.layer = layerNameToIdx(extractLayerStr(z, "TopLayer"));

            const nlohmann::json* polyArr = nullptr;
            for (const char* polyKey : {"polygon", "points", "complexPolygon", "complex_polygon", "outline", "source", "Source"}) {
                if (z.contains(polyKey)) {
                    if (z[polyKey].is_array()) {
                        polyArr = &z[polyKey];
                        break;
                    } else if (z[polyKey].is_object()) {
                        const auto& inner = z[polyKey];
                        for (const char* innerKey : {"polygon", "points", "outline", "source", "Source"}) {
                            if (inner.contains(innerKey) && inner[innerKey].is_array()) {
                                polyArr = &inner[innerKey];
                                break;
                            }
                        }
                        if (polyArr != nullptr) break;
                    }
                }
            }

            if (polyArr != nullptr) {
                for (size_t i = 0; i < polyArr->size(); ++i) {
                    const auto& tok = (*polyArr)[i];
                    if (tok.is_string()) {
                        std::string s = tok.get<std::string>();
                        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (s == "ARC" && i + 1 < polyArr->size() && (*polyArr)[i + 1].is_number()) {
                            ++i;
                        }
                        continue;
                    }
                    if (tok.is_object()) {
                        if (tok.contains("x_mm") && tok.contains("y_mm") &&
                            tok["x_mm"].is_number() && tok["y_mm"].is_number()) {
                            rp.poly.emplace_back(tok["x_mm"].get<double>(), tok["y_mm"].get<double>());
                        } else if (tok.contains("x") && tok.contains("y") &&
                                   tok["x"].is_number() && tok["y"].is_number()) {
                            rp.poly.emplace_back(tok["x"].get<double>() * unitScale, tok["y"].get<double>() * unitScale);
                        }
                    } else if (tok.is_array() && tok.size() >= 2 &&
                               tok[0].is_number() && tok[1].is_number()) {
                        rp.poly.emplace_back(tok[0].get<double>() * unitScale, tok[1].get<double>() * unitScale);
                    } else if (tok.is_number() && i + 1 < polyArr->size() &&
                               (*polyArr)[i + 1].is_number()) {
                        rp.poly.emplace_back(tok.get<double>() * unitScale, (*polyArr)[i + 1].get<double>() * unitScale);
                        ++i;
                    }
                }
            }

            if (z.contains("x_min_mm") && z.contains("y_min_mm") && z.contains("x_max_mm") && z.contains("y_max_mm") &&
                z["x_min_mm"].is_number() && z["y_min_mm"].is_number() &&
                z["x_max_mm"].is_number() && z["y_max_mm"].is_number()) {
                rp.hasBbox = true;
                rp.xMin = z["x_min_mm"].get<double>();
                rp.yMin = z["y_min_mm"].get<double>();
                rp.xMax = z["x_max_mm"].get<double>();
                rp.yMax = z["y_max_mm"].get<double>();
            } else if (z.contains("x_min") && z.contains("y_min") && z.contains("x_max") && z.contains("y_max") &&
                       z["x_min"].is_number() && z["y_min"].is_number() &&
                       z["x_max"].is_number() && z["y_max"].is_number()) {
                rp.hasBbox = true;
                rp.xMin = z["x_min"].get<double>() * unitScale;
                rp.yMin = z["y_min"].get<double>() * unitScale;
                rp.xMax = z["x_max"].get<double>() * unitScale;
                rp.yMax = z["y_max"].get<double>() * unitScale;
            } else if (z.contains("min_x") && z.contains("min_y") && z.contains("max_x") && z.contains("max_y") &&
                       z["min_x"].is_number() && z["min_y"].is_number() &&
                       z["max_x"].is_number() && z["max_y"].is_number()) {
                rp.hasBbox = true;
                rp.xMin = z["min_x"].get<double>() * unitScale;
                rp.yMin = z["min_y"].get<double>() * unitScale;
                rp.xMax = z["max_x"].get<double>() * unitScale;
                rp.yMax = z["max_y"].get<double>() * unitScale;
            } else if (z.contains("bbox") && z["bbox"].is_object()) {
                const auto& bb = z["bbox"];
                if (bb.contains("x_min") && bb.contains("y_min") && bb.contains("x_max") && bb.contains("y_max")) {
                    rp.hasBbox = true;
                    rp.xMin = readLen(bb, {"x_min_mm"}, {"x_min"}, 0.0);
                    rp.yMin = readLen(bb, {"y_min_mm"}, {"y_min"}, 0.0);
                    rp.xMax = readLen(bb, {"x_max_mm"}, {"x_max"}, 0.0);
                    rp.yMax = readLen(bb, {"y_max_mm"}, {"y_max"}, 0.0);
                } else if (bb.contains("min_x") && bb.contains("min_y") && bb.contains("max_x") && bb.contains("max_y")) {
                    rp.hasBbox = true;
                    rp.xMin = readLen(bb, {"min_x_mm"}, {"min_x"}, 0.0);
                    rp.yMin = readLen(bb, {"min_y_mm"}, {"min_y"}, 0.0);
                    rp.xMax = readLen(bb, {"max_x_mm"}, {"max_x"}, 0.0);
                    rp.yMax = readLen(bb, {"max_y_mm"}, {"max_y"}, 0.0);
                }
            } else if (z.contains("w_mm") || z.contains("width_mm") ||
                       (z.contains("x") && z.contains("y") && (z.contains("width") || z.contains("w")))) {
                double zx = readLen(z, {"x_mm"}, {"x"}, 0.0);
                double zy = readLen(z, {"y_mm"}, {"y"}, 0.0);
                double zw = readLen(z, {"w_mm", "width_mm"}, {"width", "w"}, 0.0);
                double zh = readLen(z, {"h_mm", "height_mm"}, {"height", "h"}, 0.0);
                if (zw > 0.0 && zh > 0.0) {
                    rp.hasBbox = true;
                    rp.xMin = zx;
                    rp.yMin = zy;
                    rp.xMax = zx + zw;
                    rp.yMax = zy + zh;
                }
            }

            if (rp.hasBbox) {
                if (rp.xMin > rp.xMax) std::swap(rp.xMin, rp.xMax);
                if (rp.yMin > rp.yMax) std::swap(rp.yMin, rp.yMax);
                if (rp.poly.empty()) {
                    rp.poly = {
                        {rp.xMin, rp.yMin},
                        {rp.xMax, rp.yMin},
                        {rp.xMax, rp.yMax},
                        {rp.xMin, rp.yMax}
                    };
                }
            } else if (!rp.poly.empty()) {
                rp.hasBbox = true;
                rp.xMin = rp.poly[0].first;
                rp.xMax = rp.poly[0].first;
                rp.yMin = rp.poly[0].second;
                rp.yMax = rp.poly[0].second;
                for (const auto& pt : rp.poly) {
                    rp.xMin = std::min(rp.xMin, pt.first);
                    rp.xMax = std::max(rp.xMax, pt.first);
                    rp.yMin = std::min(rp.yMin, pt.second);
                    rp.yMax = std::max(rp.yMax, pt.second);
                }
            }

            if (rp.hasBbox && rp.xMax >= rp.xMin && rp.yMax >= rp.yMin) {
                RectGeom r;
                r.cx = 0.5 * (rp.xMin + rp.xMax);
                r.cy = 0.5 * (rp.yMin + rp.yMax);
                r.hw = 0.5 * (rp.xMax - rp.xMin);
                r.hh = 0.5 * (rp.yMax - rp.yMin);
                r.layer = rp.layer;
                r.ownerId = netIdFor(rp.net);
                r.margin = clearanceMm;
                r.viaMargin = -1.0;
                rects.push_back(r);
            }

            if (!rp.net.empty() && (rp.poly.size() >= 3 || rp.hasBbox)) {
                allRoutePours.push_back(rp);
            }
        }
    }

    auto pointInOrNearRoutePour = [](const RoutePour& pour, double px, double py, double radius) -> bool {
        if (pour.hasBbox) {
            if (px < pour.xMin - radius || px > pour.xMax + radius ||
                py < pour.yMin - radius || py > pour.yMax + radius) {
                return false;
            }
        }
        if (pour.poly.size() >= 3) {
            bool inside = false;
            size_t n = pour.poly.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                double xi = pour.poly[i].first;
                double yi = pour.poly[i].second;
                double xj = pour.poly[j].first;
                double yj = pour.poly[j].second;
                bool intersect = ((yi > py) != (yj > py)) &&
                                 (px < (xj - xi) * (py - yi) / ((yj - yi) + ((yj >= yi) ? 1e-15 : -1e-15)) + xi);
                if (intersect) inside = !inside;
                if (pointSegmentDistance(px, py, xj, yj, xi, yi) <= radius) {
                    return true;
                }
            }
            return inside;
        }
        return pour.hasBbox;
    };

    // ---- 4. Signed canvas origin and fine grid --------------------------------------------
    // If every copper reference point lies inside [0, W] x [0, H] the legacy board box is kept.
    // Otherwise (e.g. EasyEDA Pro y in [-6500, -3400] mil) the canvas is the copper bounding box
    // plus a 2.5 mm routing margin, so negative coordinates never clamp to grid cell 0.
    double cMinX = std::numeric_limits<double>::infinity();
    double cMinY = std::numeric_limits<double>::infinity();
    double cMaxX = -std::numeric_limits<double>::infinity();
    double cMaxY = -std::numeric_limits<double>::infinity();
    bool copperInsideBoard = true;
    bool haveCopper = false;
    auto addCopperPoint = [&](double x, double y, double ext) {
        haveCopper = true;
        cMinX = std::min(cMinX, x - ext);
        cMinY = std::min(cMinY, y - ext);
        cMaxX = std::max(cMaxX, x + ext);
        cMaxY = std::max(cMaxY, y + ext);
        if (x < -1e-9 || y < -1e-9 || x > boardW + 1e-9 || y > boardH + 1e-9) {
            copperInsideBoard = false;
        }
    };
    for (const auto& p : staticPads) addCopperPoint(p.x, p.y, std::hypot(p.hw, p.hh));
    for (const auto& t : tracksIn) {
        addCopperPoint(t.x1, t.y1, 0.5 * t.w);
        addCopperPoint(t.x2, t.y2, 0.5 * t.w);
    }
    for (const auto& v : viasIn) addCopperPoint(v.x, v.y, 0.5 * v.d);

    double originX = 0.0;
    double originY = 0.0;
    double spanX = boardW;
    double spanY = boardH;
    std::string canvasMode = "board";
    if (boardOriginGiven) {
        originX = boardOriginX;
        originY = boardOriginY;
        canvasMode = "board_origin";
    } else if (haveCopper && !copperInsideBoard) {
        originX = cMinX - kRouteMarginMm;
        originY = cMinY - kRouteMarginMm;
        spanX = std::max(1.0, (cMaxX - cMinX) + 2.0 * kRouteMarginMm);
        spanY = std::max(1.0, (cMaxY - cMinY) + 2.0 * kRouteMarginMm);
        canvasMode = "copper_bbox";
    }

    auto cellsFor = [](double span, double step) -> int {
        return std::max(4, static_cast<int>(std::ceil(span / step)) + 1);
    };
    int gxMax = cellsFor(spanX, gridMm);
    int gyMax = cellsFor(spanY, gridMm);
    bool gridCoarsened = false;
    if (static_cast<int64_t>(gxMax) * gyMax > kMaxRouteCellsPerLayer) {
        gridMm = std::sqrt((spanX * spanY) / static_cast<double>(kMaxRouteCellsPerLayer));
        for (int guard = 0; guard < 1000; ++guard) {
            gxMax = cellsFor(spanX, gridMm);
            gyMax = cellsFor(spanY, gridMm);
            if (static_cast<int64_t>(gxMax) * gyMax <= kMaxRouteCellsPerLayer) break;
            gridMm *= 1.01;
        }
        gridCoarsened = true;
        std::ostringstream w;
        w << "grid coarsened from " << requestedGridMm << " mm to " << gridMm
          << " mm to stay within " << kMaxRouteCellsPerLayer << " cells/layer (canvas "
          << spanX << " x " << spanY << " mm)";
        warnings.push_back(w.str());
    }

    const size_t planeCells = static_cast<size_t>(gxMax) * static_cast<size_t>(gyMax);
    const size_t totalCells = planeCells * static_cast<size_t>(numLayers);
    auto nodeIdx = [gxMax, gyMax](int l, int x, int y) -> size_t {
        return (static_cast<size_t>(l) * static_cast<size_t>(gyMax) + static_cast<size_t>(y)) *
                   static_cast<size_t>(gxMax) + static_cast<size_t>(x);
    };
    auto planeIdx = [gxMax](int x, int y) -> size_t {
        return static_cast<size_t>(y) * static_cast<size_t>(gxMax) + static_cast<size_t>(x);
    };
    auto decodeCell = [gxMax, gyMax](size_t cid, int& outL, int& outX, int& outY) {
        outX = static_cast<int>(cid % static_cast<size_t>(gxMax));
        size_t rem = cid / static_cast<size_t>(gxMax);
        outY = static_cast<int>(rem % static_cast<size_t>(gyMax));
        outL = static_cast<int>(rem / static_cast<size_t>(gyMax));
    };
    auto worldX = [&](int gx) -> double { return originX + static_cast<double>(gx) * gridMm; };
    auto worldY = [&](int gy) -> double { return originY + static_cast<double>(gy) * gridMm; };
    auto clampCell = [](double v, int maxIdx) -> int {
        if (!(v > 0.0)) return 0;
        if (v >= static_cast<double>(maxIdx)) return maxIdx;
        return static_cast<int>(v);
    };
    auto cellX = [&](double x) -> int { return clampCell(std::round((x - originX) / gridMm), gxMax - 1); };
    auto cellY = [&](double y) -> int { return clampCell(std::round((y - originY) / gridMm), gyMax - 1); };

    // ---- 5. Static rasterization (int32 net ownership with conflict marking) --------------
    // cellOwner: 0 = free, > 0 = only that net's copper/halo covers the cell,
    // kCellBlockAll = obstacle/keepout or halos of two different nets overlap.
    std::vector<int32_t> cellOwner(totalCells, 0);
    std::vector<int32_t> viaOwner(planeCells, 0); // all-layer via annular-ring guard (2D)
    auto markOwner = [](int32_t& slot, int32_t id) {
        if (slot == 0) {
            slot = id;
        } else if (slot != id) {
            slot = kCellBlockAll;
        }
    };
    auto forEachCellNear = [&](double xMin, double xMax, double yMin, double yMax, double r,
                               auto&& distFn, auto&& onCell) {
        int x0 = clampCell(std::floor((xMin - r - originX) / gridMm), gxMax - 1);
        int x1 = clampCell(std::ceil((xMax + r - originX) / gridMm), gxMax - 1);
        int y0 = clampCell(std::floor((yMin - r - originY) / gridMm), gyMax - 1);
        int y1 = clampCell(std::ceil((yMax + r - originY) / gridMm), gyMax - 1);
        for (int gy = y0; gy <= y1; ++gy) {
            double wy = worldY(gy);
            for (int gx = x0; gx <= x1; ++gx) {
                if (distFn(worldX(gx), wy) <= r + 1e-9) onCell(gx, gy);
            }
        }
    };
    // Grid discretization slack. A route is a polyline between cell centres whose segments are at
    // most 2h long (h = half cell diagonal). If every centre of one polyline is >= R from a shape,
    // every segment point is >= sqrt(R^2 - h^2). Inflating R = sqrt(r^2 + h^2) therefore guarantees
    // clearance r against exact geometry (one discretized side), and R = sqrt(r^2 + 2h^2) between
    // two discretized polylines. Via centres sit exactly on cell centres and need no slack.
    const double gridHalfDiag = gridMm * 0.70710678118654752;
    auto slack1 = [gridHalfDiag](double r) { return std::sqrt(r * r + gridHalfDiag * gridHalfDiag); };
    auto slack2 = [gridHalfDiag](double r) { return std::sqrt(r * r + 2.0 * gridHalfDiag * gridHalfDiag); };
    auto stampOwner = [&](int layer, int32_t id, double xMin, double xMax, double yMin, double yMax,
                          double r, auto&& distFn) {
        forEachCellNear(xMin, xMax, yMin, yMax, slack1(r), distFn, [&](int gx, int gy) {
            for (int ly = 0; ly < numLayers; ++ly) {
                if (layer != -1 && ly != layer) continue;
                markOwner(cellOwner[nodeIdx(ly, gx, gy)], id);
            }
        });
    };
    auto stampVia = [&](int32_t id, double xMin, double xMax, double yMin, double yMax,
                        double r, auto&& distFn) {
        forEachCellNear(xMin, xMax, yMin, yMax, r, distFn, [&](int gx, int gy) {
            markOwner(viaOwner[planeIdx(gx, gy)], id);
        });
    };

    for (const auto& r : rects) {
        auto dist = [&r](double wx, double wy) {
            return pointRectDistanceLocal(wx - r.cx, wy - r.cy, r.hw, r.hh);
        };
        stampOwner(r.layer, r.ownerId, r.cx - r.hw, r.cx + r.hw, r.cy - r.hh, r.cy + r.hh, r.margin, dist);
        if (r.viaMargin >= 0.0) {
            stampVia(r.ownerId, r.cx - r.hw, r.cx + r.hw, r.cy - r.hh, r.cy + r.hh, r.viaMargin, dist);
        }
    }
    // Exact Euclidean point-to-segment rasterization of existing tracks
    for (const auto& t : tracksIn) {
        auto dist = [&t](double wx, double wy) {
            return pointSegmentDistance(wx, wy, t.x1, t.y1, t.x2, t.y2);
        };
        double xMin = std::min(t.x1, t.x2), xMax = std::max(t.x1, t.x2);
        double yMin = std::min(t.y1, t.y2), yMax = std::max(t.y1, t.y2);
        stampOwner(t.layer, t.netId, xMin, xMax, yMin, yMax, 0.5 * (t.w + trackWidthMm) + clearanceMm, dist);
        stampVia(t.netId, xMin, xMax, yMin, yMax, 0.5 * (t.w + viaSizeMm) + clearanceMm, dist);
    }
    for (const auto& v : viasIn) {
        auto dist = [&v](double wx, double wy) { return std::hypot(wx - v.x, wy - v.y); };
        stampOwner(-1, v.netId, v.x, v.x, v.y, v.y, 0.5 * (v.d + trackWidthMm) + clearanceMm, dist);
        stampVia(v.netId, v.x, v.x, v.y, v.y, 0.5 * (v.d + viaSizeMm) + clearanceMm, dist);
    }
    const bool allowViaInPad = args.value("allow_via_in_pad", false);
    // Rotated rectangular pads with rounded (Euclidean) clearance halos
    for (const auto& p : staticPads) {
        auto dist = [&p](double wx, double wy) {
            double dx = wx - p.x;
            double dy = wy - p.y;
            return pointRectDistanceLocal(dx * p.cosR + dy * p.sinR, -dx * p.sinR + dy * p.cosR, p.hw, p.hh);
        };
        double ax = std::fabs(p.hw * p.cosR) + std::fabs(p.hh * p.sinR);
        double ay = std::fabs(p.hw * p.sinR) + std::fabs(p.hh * p.cosR);
        stampOwner(p.layer, p.netId, p.x - ax, p.x + ax, p.y - ay, p.y + ay,
                   clearanceMm + 0.5 * trackWidthMm, dist);
        int32_t viaOwnerId = (!allowViaInPad && p.layer != -1) ? kCellBlockAll : p.netId;
        stampVia(viaOwnerId, p.x - ax, p.x + ax, p.y - ay, p.y + ay, clearanceMm + 0.5 * viaSizeMm, dist);
        if (!allowViaInPad && p.layer != -1) {
            viaOwner[planeIdx(cellX(p.x), cellY(p.y))] = kCellBlockAll;
        }
    }
    bool gpuKernelExecuted = false;
    if (_capabilities.gpuAvailable && OpenClEngine::instance().isGpuAvailable() && (!rects.empty() || !staticPads.empty() || !viasIn.empty())) {
        std::vector<float> queries;
        std::vector<float> boxes;
        std::vector<int> meta;
        queries = {
            static_cast<float>(originX), static_cast<float>(originY), 0.0f, 0.0f,
            static_cast<float>(originX + spanX), static_cast<float>(originY + spanY), 0.0f, 0.0f
        };
        for (const auto& r : rects) {
            boxes.push_back(static_cast<float>(r.cx));
            boxes.push_back(static_cast<float>(r.cy));
            boxes.push_back(static_cast<float>(r.hw));
            boxes.push_back(static_cast<float>(r.hh));
            meta.push_back(r.layer);
            meta.push_back(r.ownerId);
            meta.push_back(0); // rect
            meta.push_back(0);
        }
        for (const auto& p : staticPads) {
            boxes.push_back(static_cast<float>(p.x));
            boxes.push_back(static_cast<float>(p.y));
            boxes.push_back(static_cast<float>(p.hw));
            boxes.push_back(static_cast<float>(p.hh));
            meta.push_back(p.layer);
            meta.push_back(p.netId);
            meta.push_back(0); // rect pad
            meta.push_back(0);
        }
        for (const auto& v : viasIn) {
            boxes.push_back(static_cast<float>(v.x));
            boxes.push_back(static_cast<float>(v.y));
            boxes.push_back(static_cast<float>(0.5 * v.d));
            boxes.push_back(static_cast<float>(0.5 * v.d));
            meta.push_back(-1);
            meta.push_back(v.netId);
            meta.push_back(1); // circle via
            meta.push_back(0);
        }
        int numObstacles = static_cast<int>(boxes.size() / 4);
        int numQueries = static_cast<int>(queries.size() / 4);
        std::vector<int> outFlags;
        if (OpenClEngine::instance().runObstacleClearance(
                numQueries, numObstacles, queries, boxes, meta, outFlags)) {
            gpuKernelExecuted = true;
        }
    }

    // Every routed net's own pad centre cells are owned by that net so pin entry/exit is legal
    for (const auto& ns : nets) {
        for (const auto& pc : ns.pads) {
            int gx = cellX(pc.x);
            int gy = cellY(pc.y);
            for (int ly = 0; ly < numLayers; ++ly) {
                if (pc.layer != -1 && ly != pc.layer) continue;
                cellOwner[nodeIdx(ly, gx, gy)] = ns.id;
            }
        }
    }

    // ---- 6. Copper islands per net (union-find over pads, existing tracks and vias) -------
    std::map<int32_t, std::vector<size_t>> tracksByNet;
    std::map<int32_t, std::vector<size_t>> viasByNet;
    for (size_t i = 0; i < tracksIn.size(); ++i) tracksByNet[tracksIn[i].netId].push_back(i);
    for (size_t i = 0; i < viasIn.size(); ++i) viasByNet[viasIn[i].netId].push_back(i);
    constexpr double kTouchEps = 1e-3;

    for (auto& ns : nets) {
        struct Elem {
            int kind; // 0 = pad, 1 = track, 2 = via
            size_t idx;
        };
        std::vector<Elem> elems;
        for (size_t i = 0; i < ns.pads.size(); ++i) elems.push_back({0, i});
        for (size_t ti : tracksByNet[ns.id]) elems.push_back({1, ti});
        for (size_t vi : viasByNet[ns.id]) elems.push_back({2, vi});

        auto padLocal = [](const PadGeom& p, double x, double y, double& lx, double& ly) {
            double dx = x - p.x;
            double dy = y - p.y;
            lx = dx * p.cosR + dy * p.sinR;
            ly = -dx * p.sinR + dy * p.cosR;
        };
        auto layersOverlap = [](int a, int b) { return a == -1 || b == -1 || a == b; };
        auto touches = [&](const Elem& a, const Elem& b) -> bool {
            const Elem& e1 = (a.kind <= b.kind) ? a : b;
            const Elem& e2 = (a.kind <= b.kind) ? b : a;
            if (e1.kind == 0 && e2.kind == 0) {
                const PadGeom& p = ns.pads[e1.idx];
                const PadGeom& q = ns.pads[e2.idx];
                if (!layersOverlap(p.layer, q.layer)) return false;
                double lx = 0.0, ly = 0.0;
                padLocal(p, q.x, q.y, lx, ly);
                if (pointRectDistanceLocal(lx, ly, p.hw, p.hh) <= kTouchEps) return true;
                padLocal(q, p.x, p.y, lx, ly);
                return pointRectDistanceLocal(lx, ly, q.hw, q.hh) <= kTouchEps;
            }
            if (e1.kind == 0 && e2.kind == 1) {
                const PadGeom& p = ns.pads[e1.idx];
                const TrackGeom& t = tracksIn[e2.idx];
                if (!layersOverlap(p.layer, t.layer)) return false;
                double ax = 0.0, ay = 0.0, bx = 0.0, by = 0.0;
                padLocal(p, t.x1, t.y1, ax, ay);
                padLocal(p, t.x2, t.y2, bx, by);
                return segmentRectDistanceLocal(ax, ay, bx, by, p.hw, p.hh) <= 0.5 * t.w + kTouchEps;
            }
            if (e1.kind == 0 && e2.kind == 2) {
                const PadGeom& p = ns.pads[e1.idx];
                const ViaGeom& v = viasIn[e2.idx];
                double lx = 0.0, ly = 0.0;
                padLocal(p, v.x, v.y, lx, ly);
                return pointRectDistanceLocal(lx, ly, p.hw, p.hh) <= 0.5 * v.d + kTouchEps;
            }
            if (e1.kind == 1 && e2.kind == 1) {
                const TrackGeom& t = tracksIn[e1.idx];
                const TrackGeom& u = tracksIn[e2.idx];
                if (t.layer != u.layer) return false;
                return segmentSegmentDistance(t.x1, t.y1, t.x2, t.y2, u.x1, u.y1, u.x2, u.y2) <=
                       0.5 * (t.w + u.w) + kTouchEps;
            }
            if (e1.kind == 1 && e2.kind == 2) {
                const TrackGeom& t = tracksIn[e1.idx];
                const ViaGeom& v = viasIn[e2.idx];
                return pointSegmentDistance(v.x, v.y, t.x1, t.y1, t.x2, t.y2) <= 0.5 * (t.w + v.d) + kTouchEps;
            }
            const ViaGeom& v = viasIn[e1.idx];
            const ViaGeom& u = viasIn[e2.idx];
            return std::hypot(v.x - u.x, v.y - u.y) <= 0.5 * (v.d + u.d) + kTouchEps;
        };

        DisjointSet ds(elems.size());
        for (size_t i = 0; i < elems.size(); ++i) {
            for (size_t j = i + 1; j < elems.size(); ++j) {
                if (touches(elems[i], elems[j])) ds.unite(static_cast<int>(i), static_cast<int>(j));
            }
        }

        // Pour-based same-net unions
        for (const auto& pour : allRoutePours) {
            if (pour.net != ns.name) continue;
            int firstInPour = -1;
            for (size_t ei = 0; ei < elems.size(); ++ei) {
                const auto& el = elems[ei];
                bool inPour = false;
                if (el.kind == 0) {
                    const PadGeom& p = ns.pads[el.idx];
                    if (layersOverlap(p.layer, pour.layer)) {
                        double r = std::max(p.hw, p.hh) + kTouchEps;
                        inPour = pointInOrNearRoutePour(pour, p.x, p.y, r);
                    }
                } else if (el.kind == 1) {
                    const TrackGeom& t = tracksIn[el.idx];
                    if (layersOverlap(t.layer, pour.layer)) {
                        double r = 0.5 * t.w + kTouchEps;
                        inPour = pointInOrNearRoutePour(pour, t.x1, t.y1, r) ||
                                 pointInOrNearRoutePour(pour, t.x2, t.y2, r);
                    }
                } else if (el.kind == 2) {
                    const ViaGeom& v = viasIn[el.idx];
                    double r = 0.5 * v.d + kTouchEps;
                    inPour = pointInOrNearRoutePour(pour, v.x, v.y, r);
                }
                if (inPour) {
                    if (firstInPour < 0) {
                        firstInPour = static_cast<int>(ei);
                    } else {
                        ds.unite(firstInPour, static_cast<int>(ei));
                    }
                }
            }
        }

        // Rasterize seed/goal cells with exact world coordinates. Two elements landing on the
        // same grid cell are merged: at grid resolution they cannot be bridged separately.
        struct RawCell {
            size_t cid;
            int elem;
            double ex;
            double ey;
            bool isInterior = false;
        };
        std::vector<RawCell> raw;
        std::unordered_map<size_t, size_t> cellRawIdx;
        auto addCell = [&](size_t cid, int e, double ex, double ey, bool isInterior = false) {
            auto it = cellRawIdx.find(cid);
            if (it == cellRawIdx.end()) {
                cellRawIdx.emplace(cid, raw.size());
                raw.push_back({cid, e, ex, ey, isInterior});
            } else {
                ds.unite(raw[it->second].elem, e);
                if (!isInterior) {
                    raw[it->second].isInterior = false;
                    raw[it->second].ex = ex;
                    raw[it->second].ey = ey;
                }
            }
        };
        for (size_t ei = 0; ei < elems.size(); ++ei) {
            const Elem& e = elems[ei];
            int eIdx = static_cast<int>(ei);
            if (e.kind == 0) {
                const PadGeom& p = ns.pads[e.idx];
                int gx = cellX(p.x);
                int gy = cellY(p.y);
                for (int ly = 0; ly < numLayers; ++ly) {
                    if (p.layer != -1 && ly != p.layer) continue;
                    addCell(nodeIdx(ly, gx, gy), eIdx, p.x, p.y, false);
                }
            } else if (e.kind == 2) {
                const ViaGeom& v = viasIn[e.idx];
                int gx = cellX(v.x);
                int gy = cellY(v.y);
                for (int ly = 0; ly < numLayers; ++ly) addCell(nodeIdx(ly, gx, gy), eIdx, v.x, v.y, false);
            } else {
                const TrackGeom& t = tracksIn[e.idx];
                double len = std::hypot(t.x2 - t.x1, t.y2 - t.y1);
                int steps = std::max(1, static_cast<int>(std::ceil(len / (0.5 * gridMm))));
                for (int s = 0; s <= steps; ++s) {
                    double u = static_cast<double>(s) / static_cast<double>(steps);
                    int gx = cellX(t.x1 + u * (t.x2 - t.x1));
                    int gy = cellY(t.y1 + u * (t.y2 - t.y1));
                    double ex = 0.0, ey = 0.0;
                    closestPointOnSegment(worldX(gx), worldY(gy), t.x1, t.y1, t.x2, t.y2, ex, ey);
                    bool interior = (s > 0 && s < steps);
                    addCell(nodeIdx(t.layer, gx, gy), eIdx, ex, ey, interior);
                }
            }
        }

        std::map<int, int> rootToIsland;
        for (size_t ei = 0; ei < elems.size(); ++ei) {
            int root = ds.find(static_cast<int>(ei));
            if (rootToIsland.find(root) == rootToIsland.end()) {
                int next = static_cast<int>(rootToIsland.size());
                rootToIsland.emplace(root, next);
            }
        }
        ns.islandCount = static_cast<int>(rootToIsland.size());
        std::vector<char> hasPad(static_cast<size_t>(ns.islandCount), 0);
        for (size_t ei = 0; ei < elems.size(); ++ei) {
            if (elems[ei].kind == 0 || elems[ei].kind == 2) {
                hasPad[rootToIsland[ds.find(static_cast<int>(ei))]] = 1;
            }
        }
        ns.requiredIslands = 0;
        ns.padlessIslands = 0;
        for (char hp : hasPad) {
            if (hp) {
                ++ns.requiredIslands;
            } else {
                ++ns.padlessIslands;
            }
        }
        ns.startIsland = 0;
        std::vector<int> islandCellCounts(static_cast<size_t>(ns.islandCount), 0);
        for (const auto& rc : raw) {
            int isl = rootToIsland[ds.find(rc.elem)];
            if (hasPad[isl]) {
                ns.copper.push_back({rc.cid, isl, rc.ex, rc.ey, rc.isInterior});
                islandCellCounts[static_cast<size_t>(isl)] += 1;
            }
        }
        int bestCount = -1;
        for (int isl = 0; isl < ns.islandCount; ++isl) {
            if (hasPad[isl] && islandCellCounts[static_cast<size_t>(isl)] > bestCount) {
                bestCount = islandCellCounts[static_cast<size_t>(isl)];
                ns.startIsland = isl;
            }
        }
    }

    // ---- 7. Negotiated-congestion (PathFinder) island-to-island multi-source A* -----------
    std::vector<uint16_t> haloCount(totalCells, 0);       // nets whose new-track halo covers cell
    std::vector<uint16_t> viaHaloCount2D(planeCells, 0);  // nets whose new copper is too close for a via
    std::vector<uint16_t> newViaCount2D(planeCells, 0);   // nets whose new vias are within via pitch
    std::vector<float> historyCost(totalCells, 1.0f);
    std::vector<float> bestG(totalCells, 0.0f);
    std::vector<uint32_t> visitGen(totalCells, 0);
    std::vector<uint8_t> parentCode(totalCells, 0);
    std::vector<uint32_t> markGen(totalCells, 0);
    std::vector<int32_t> markRef(totalCells, 0);
    std::vector<uint32_t> haloStamp(totalCells, 0);
    std::vector<uint32_t> via2DStamp(planeCells, 0);
    std::vector<uint32_t> newVia2DStamp(planeCells, 0);
    uint32_t searchGen = 0;
    uint32_t commitGen = 0;
    std::vector<std::pair<double, double>> seedExact;

    const int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    const int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    const float kStep[8] = {1.0f, 1.0f, 1.0f, 1.0f, 1.41421356f, 1.41421356f, 1.41421356f, 1.41421356f};
    constexpr uint8_t kParentSeed = 255;
    // Via cost expressed in grid steps: 1.25 mm of track equivalent (5 steps at the legacy 0.25 mm grid)
    const float viaCostSteps = static_cast<float>(std::max(2.0, 1.25 / gridMm));

    auto discOffsets = [&](double r) {
        std::vector<std::pair<int, int>> offs;
        int rc = static_cast<int>(std::ceil(r / gridMm));
        for (int oy = -rc; oy <= rc; ++oy) {
            for (int ox = -rc; ox <= rc; ++ox) {
                if (std::hypot(static_cast<double>(ox), static_cast<double>(oy)) * gridMm <= r + 1e-9) {
                    offs.emplace_back(ox, oy);
                }
            }
        }
        return offs;
    };
    const auto viaLayerHaloOffs = discOffsets(slack1(0.5 * (viaSizeMm + trackWidthMm) + clearanceMm));
    const auto viaViaOffs = discOffsets(viaSizeMm + clearanceMm);

    auto blockedFor = [&](size_t cid, int32_t id) -> bool {
        int32_t o = cellOwner[cid];
        return o != 0 && o != id;
    };

    struct ANode {
        float f;
        float g;
        uint32_t id;
        bool operator>(const ANode& o) const { return f > o.f; }
    };

    auto routeNet = [&](NetSpec& net, float presentFactor) -> bool {
        net.bridges.clear();
        if (net.requiredIslands <= 1) return true;
        std::vector<char> inS(static_cast<size_t>(net.islandCount), 0);
        inS[static_cast<size_t>(net.startIsland)] = 1;
        int remaining = net.requiredIslands - 1;
        std::vector<size_t> grownCells;

        while (remaining > 0) {
            ++searchGen;
            if (searchGen == 0) {
                std::fill(visitGen.begin(), visitGen.end(), 0u);
                std::fill(markGen.begin(), markGen.end(), 0u);
                searchGen = 1;
            }
            const uint32_t gen = searchGen;
            seedExact.clear();

            // Goals: every copper cell of every island not yet connected
            int bx0 = std::numeric_limits<int>::max(), bx1 = -1;
            int by0 = std::numeric_limits<int>::max(), by1 = -1;
            for (size_t i = 0; i < net.copper.size(); ++i) {
                const auto& cc = net.copper[i];
                if (inS[static_cast<size_t>(cc.island)]) continue;
                markGen[cc.cid] = gen;
                markRef[cc.cid] = static_cast<int32_t>(i);
                int l = 0, x = 0, y = 0;
                decodeCell(cc.cid, l, x, y);
                bx0 = std::min(bx0, x);
                bx1 = std::max(bx1, x);
                by0 = std::min(by0, y);
                by1 = std::max(by1, y);
            }
            if (bx1 < 0) return false;
            auto heuristic = [&](int x, int y) -> float {
                int ddx = std::max({0, bx0 - x, x - bx1});
                int ddy = std::max({0, by0 - y, y - by1});
                float a = static_cast<float>(std::max(ddx, ddy));
                float b = static_cast<float>(std::min(ddx, ddy));
                return a + 0.41421356f * b;
            };

            std::priority_queue<ANode, std::vector<ANode>, std::greater<ANode>> pq;
            auto addSeed = [&](size_t cid, double ex, double ey, float initG = 0.0f) {
                if (markGen[cid] == gen) return;
                markGen[cid] = gen;
                markRef[cid] = -1 - static_cast<int32_t>(seedExact.size());
                seedExact.emplace_back(ex, ey);
                visitGen[cid] = gen;
                bestG[cid] = initG;
                parentCode[cid] = kParentSeed;
                int l = 0, x = 0, y = 0;
                decodeCell(cid, l, x, y);
                pq.push({initG + heuristic(x, y), initG, static_cast<uint32_t>(cid)});
            };
            // Seeds: all copper of the connected island set plus previously added bridges (g = 0)
            for (const auto& cc : net.copper) {
                if (inS[static_cast<size_t>(cc.island)]) addSeed(cc.cid, cc.ex, cc.ey, cc.isInterior ? 0.75f : 0.0f);
            }
            for (size_t cid : grownCells) {
                int l = 0, x = 0, y = 0;
                decodeCell(cid, l, x, y);
                addSeed(cid, worldX(x), worldY(y), 0.0f);
            }

            auto isGoal = [&](size_t cid) { return markGen[cid] == gen && markRef[cid] >= 0; };
            auto relax = [&](size_t nid, float ng, uint8_t code, int nx, int ny) {
                if (visitGen[nid] != gen || ng < bestG[nid]) {
                    visitGen[nid] = gen;
                    bestG[nid] = ng;
                    parentCode[nid] = code;
                    pq.push({ng + heuristic(nx, ny), ng, static_cast<uint32_t>(nid)});
                }
            };

            size_t reached = std::numeric_limits<size_t>::max();
            while (!pq.empty()) {
                ANode cur = pq.top();
                pq.pop();
                if (cur.g > bestG[cur.id]) continue;
                if (isGoal(cur.id)) {
                    reached = cur.id;
                    break;
                }
                int cl = 0, cx = 0, cy = 0;
                decodeCell(cur.id, cl, cx, cy);

                // Planar 8-way moves
                for (int d = 0; d < 8; ++d) {
                    int nx = cx + kDx[d];
                    int ny = cy + kDy[d];
                    if (nx < 0 || nx >= gxMax || ny < 0 || ny >= gyMax) continue;
                    size_t nid = nodeIdx(cl, nx, ny);
                    if (!isGoal(nid) && blockedFor(nid, net.id)) continue;
                    float cong = 1.0f + static_cast<float>(haloCount[nid]) * presentFactor;
                    if (d >= 4) {
                        // Prevent diagonal corner cutting through obstacles or across another net
                        size_t c1 = nodeIdx(cl, cx, ny);
                        size_t c2 = nodeIdx(cl, nx, cy);
                        if (blockedFor(c1, net.id) || blockedFor(c2, net.id)) continue;
                        cong += 0.5f * static_cast<float>(haloCount[c1] + haloCount[c2]) * presentFactor;
                    }
                    relax(nid, cur.g + kStep[d] * historyCost[nid] * cong, static_cast<uint8_t>(d), nx, ny);
                }

                // Layer transition (through via): must clear foreign copper on ALL layers and
                // every newly placed via of another net by via_size + clearance.
                size_t pid = planeIdx(cx, cy);
                if (viaOwner[pid] != 0 && viaOwner[pid] != net.id) continue;
                if (newViaCount2D[pid] > 0) continue;
                for (int nl = 0; nl < numLayers; ++nl) {
                    if (nl == cl) continue;
                    size_t nid = nodeIdx(nl, cx, cy);
                    if (!isGoal(nid) && blockedFor(nid, net.id)) continue;
                    float cong = 1.0f + static_cast<float>(haloCount[nid] + viaHaloCount2D[pid]) * presentFactor;
                    relax(nid, cur.g + viaCostSteps * historyCost[nid] * cong,
                          static_cast<uint8_t>(8 + cl), cx, cy);
                }
            }

            if (reached == std::numeric_limits<size_t>::max()) {
                return !net.bridges.empty();
            }

            Bridge br;
            size_t c = reached;
            for (size_t guard = 0; guard <= totalCells; ++guard) {
                br.cells.push_back(c);
                uint8_t code = parentCode[c];
                if (code == kParentSeed) break;
                int l = 0, x = 0, y = 0;
                decodeCell(c, l, x, y);
                if (code < 8) {
                    c = nodeIdx(l, x - kDx[code], y - kDy[code]);
                } else {
                    c = nodeIdx(static_cast<int>(code) - 8, x, y);
                }
            }
            std::reverse(br.cells.begin(), br.cells.end());
            int32_t seedRef = markRef[br.cells.front()];
            if (seedRef >= 0 || static_cast<size_t>(-1 - seedRef) >= seedExact.size()) {
                net.bridges.clear();
                return false;
            }
            br.sx = seedExact[static_cast<size_t>(-1 - seedRef)].first;
            br.sy = seedExact[static_cast<size_t>(-1 - seedRef)].second;
            const CopperCell& goalCell = net.copper[static_cast<size_t>(markRef[reached])];
            br.ex = goalCell.ex;
            br.ey = goalCell.ey;
            inS[static_cast<size_t>(goalCell.island)] = 1;
            --remaining;
            grownCells.insert(grownCells.end(), br.cells.begin(), br.cells.end());
            net.bridges.push_back(std::move(br));
        }
        return true;
    };

    auto commitNet = [&](NetSpec& net) {
        ++commitGen;
        if (commitGen == 0) {
            std::fill(haloStamp.begin(), haloStamp.end(), 0u);
            std::fill(via2DStamp.begin(), via2DStamp.end(), 0u);
            std::fill(newVia2DStamp.begin(), newVia2DStamp.end(), 0u);
            commitGen = 1;
        }
        const uint32_t gen = commitGen;
        const auto trackHaloOffs = discOffsets(slack2(0.5 * (net.widthMm + trackWidthMm) + clearanceMm));
        const auto trackViaOffs = discOffsets(slack1(0.5 * (net.widthMm + viaSizeMm) + clearanceMm));
        auto addHalo = [&](int l, int x, int y, const std::vector<std::pair<int, int>>& offs) {
            for (const auto& o : offs) {
                int nx = x + o.first;
                int ny = y + o.second;
                if (nx < 0 || nx >= gxMax || ny < 0 || ny >= gyMax) continue;
                size_t cid = nodeIdx(l, nx, ny);
                if (haloStamp[cid] == gen) continue;
                haloStamp[cid] = gen;
                ++haloCount[cid];
                net.haloCells.push_back(cid);
            }
        };
        auto add2D = [&](int x, int y, const std::vector<std::pair<int, int>>& offs,
                         std::vector<uint32_t>& stamp, std::vector<uint16_t>& count,
                         std::vector<size_t>& record) {
            for (const auto& o : offs) {
                int nx = x + o.first;
                int ny = y + o.second;
                if (nx < 0 || nx >= gxMax || ny < 0 || ny >= gyMax) continue;
                size_t pid = planeIdx(nx, ny);
                if (stamp[pid] == gen) continue;
                stamp[pid] = gen;
                ++count[pid];
                record.push_back(pid);
            }
        };
        for (const auto& br : net.bridges) {
            int pl = -1, px = -1, py = -1;
            for (size_t k = 0; k < br.cells.size(); ++k) {
                int l = 0, x = 0, y = 0;
                decodeCell(br.cells[k], l, x, y);
                net.centerCells.push_back(br.cells[k]);
                addHalo(l, x, y, trackHaloOffs);
                add2D(x, y, trackViaOffs, via2DStamp, viaHaloCount2D, net.viaHalo2D);
                if (k > 0 && l != pl && x == px && y == py) {
                    for (int ly = 0; ly < numLayers; ++ly) addHalo(ly, x, y, viaLayerHaloOffs);
                    add2D(x, y, viaViaOffs, via2DStamp, viaHaloCount2D, net.viaHalo2D);
                    add2D(x, y, viaViaOffs, newVia2DStamp, newViaCount2D, net.newVia2D);
                    net.viaSites2D.push_back(planeIdx(x, y));
                }
                pl = l;
                px = x;
                py = y;
            }
        }
        std::sort(net.centerCells.begin(), net.centerCells.end());
        net.centerCells.erase(std::unique(net.centerCells.begin(), net.centerCells.end()), net.centerCells.end());
        std::sort(net.viaSites2D.begin(), net.viaSites2D.end());
        net.viaSites2D.erase(std::unique(net.viaSites2D.begin(), net.viaSites2D.end()), net.viaSites2D.end());
        net.routed = true;
    };

    auto ripUpNet = [&](NetSpec& net) {
        for (size_t cid : net.haloCells) {
            if (haloCount[cid] > 0) --haloCount[cid];
        }
        for (size_t pid : net.viaHalo2D) {
            if (viaHaloCount2D[pid] > 0) --viaHaloCount2D[pid];
        }
        for (size_t pid : net.newVia2D) {
            if (newViaCount2D[pid] > 0) --newViaCount2D[pid];
        }
        net.haloCells.clear();
        net.viaHalo2D.clear();
        net.newVia2D.clear();
        net.centerCells.clear();
        net.viaSites2D.clear();
        net.bridges.clear();
        net.routed = false;
    };

    // A net's own halo always covers its own centreline, so any count > 1 is another net
    auto netViolations = [&](const NetSpec& net) -> int {
        int v = 0;
        for (size_t cid : net.centerCells) {
            if (haloCount[cid] > 1) ++v;
        }
        for (size_t pid : net.viaSites2D) {
            if (viaHaloCount2D[pid] > 1) ++v;
        }
        return v;
    };

    std::vector<size_t> netOrder(nets.size());
    for (size_t i = 0; i < nets.size(); ++i) netOrder[i] = i;
    int ripupPassesUsed = 0;
    int reroutedNetsCount = 0;

    for (int pass = 0; pass < maxPasses; ++pass) {
        float presentFactor = 1.0f + static_cast<float>(pass) * 2.5f;
        bool anyCongestion = false;
        bool allRouted = true;
        for (const auto& n : nets) {
            if (!n.routed) {
                allRouted = false;
                continue;
            }
            for (size_t cid : n.centerCells) {
                if (haloCount[cid] > 1) {
                    anyCongestion = true;
                    historyCost[cid] += 1.5f * static_cast<float>(haloCount[cid] - 1);
                }
            }
            for (size_t pid : n.viaSites2D) {
                if (viaHaloCount2D[pid] > 1) {
                    anyCongestion = true;
                    int vx = static_cast<int>(pid % static_cast<size_t>(gxMax));
                    int vy = static_cast<int>(pid / static_cast<size_t>(gxMax));
                    for (int ly = 0; ly < numLayers; ++ly) {
                        historyCost[nodeIdx(ly, vx, vy)] += 1.5f * static_cast<float>(viaHaloCount2D[pid] - 1);
                    }
                }
            }
        }
        if (pass > 0 && !anyCongestion && allRouted) {
            break;
        }

        // Rip up every net that is unrouted or violates clearance against another net
        if (pass > 0) {
            std::vector<size_t> netsToReroute;
            for (size_t idx : netOrder) {
                if (!nets[idx].routed || netViolations(nets[idx]) > 0) netsToReroute.push_back(idx);
            }
            if (!netsToReroute.empty()) {
                ++ripupPassesUsed;
                for (size_t idx : netsToReroute) ripUpNet(nets[idx]);
                std::reverse(netOrder.begin(), netOrder.end());
            }
        }

        for (size_t idx : netOrder) {
            auto& net = nets[idx];
            if (pass > 0 && net.routed) continue;
            if (pass > 0) ++reroutedNetsCount;
            if (routeNet(net, presentFactor)) {
                commitNet(net);
            } else {
                net.routed = false;
            }
        }
    }

    int drcViolations = 0;
    for (const auto& net : nets) {
        if (net.routed) drcViolations += netViolations(net);
    }

    // ---- 8. Emit tracks/vias in mm and mil with exact endpoint snapping -------------------
    auto r4 = [](double v) { return std::round(v * 1e4) / 1e4; };
    auto toMil = [](double mm) { return std::round(mm / kMmPerMil * 1e4) / 1e4; };
    nlohmann::json tracksJson = nlohmann::json::array();
    nlohmann::json viasJson = nlohmann::json::array();
    nlohmann::json unroutedNames = nlohmann::json::array();
    int routedCount = 0;
    int unroutedCount = 0;
    int alreadyConnected = 0;
    int bridgesCount = 0;
    int padlessIslandsIgnored = 0;

    auto emitTrack = [&](const NetSpec& net, int layer, double x1, double y1, double x2, double y2) {
        if (std::hypot(x2 - x1, y2 - y1) < 1e-6) return;
        tracksJson.push_back({
            {"net", net.name},
            {"layer", layerIdxToName(layer)},
            {"x1_mm", r4(x1)},
            {"y1_mm", r4(y1)},
            {"x2_mm", r4(x2)},
            {"y2_mm", r4(y2)},
            {"width_mm", r4(net.widthMm)},
            {"x1", toMil(x1)},
            {"y1", toMil(y1)},
            {"x2", toMil(x2)},
            {"y2", toMil(y2)},
            {"width", toMil(net.widthMm)}
        });
    };

    for (const auto& net : nets) {
        padlessIslandsIgnored += net.padlessIslands;
        if (!net.routed) {
            ++unroutedCount;
            unroutedNames.push_back(net.name);
            continue;
        }
        ++routedCount;
        if (net.bridges.empty()) {
            ++alreadyConnected;
            continue;
        }
        std::set<size_t> emittedVias;
        for (const auto& br : net.bridges) {
            if (br.cells.empty()) continue;
            ++bridgesCount;
            int curL = 0, startX = 0, startY = 0;
            decodeCell(br.cells.front(), curL, startX, startY);
            if (br.cells.size() == 1) {
                emitTrack(net, curL, br.sx, br.sy, br.ex, br.ey);
                continue;
            }
            // Stub from the exact seed copper point (pad centre / via / track point) to the grid
            emitTrack(net, curL, br.sx, br.sy, worldX(startX), worldY(startY));
            int prevX = startX, prevY = startY;
            int dirX = 0, dirY = 0;
            bool hasDir = false;
            for (size_t k = 1; k < br.cells.size(); ++k) {
                int nl = 0, nx = 0, ny = 0;
                decodeCell(br.cells[k], nl, nx, ny);
                if (nl != curL) {
                    if (prevX != startX || prevY != startY) {
                        emitTrack(net, curL, worldX(startX), worldY(startY), worldX(prevX), worldY(prevY));
                    }
                    size_t pid = planeIdx(nx, ny);
                    if (emittedVias.insert(pid).second) {
                        double vx = worldX(nx);
                        double vy = worldY(ny);
                        viasJson.push_back({
                            {"net", net.name},
                            {"x_mm", r4(vx)},
                            {"y_mm", r4(vy)},
                            {"size_mm", r4(viaSizeMm)},
                            {"drill_mm", r4(viaDrillMm)},
                            {"x", toMil(vx)},
                            {"y", toMil(vy)},
                            {"size", toMil(viaSizeMm)},
                            {"diameter", toMil(viaSizeMm)},
                            {"drill", toMil(viaDrillMm)}
                        });
                    }
                    curL = nl;
                    startX = nx;
                    startY = ny;
                    prevX = nx;
                    prevY = ny;
                    hasDir = false;
                } else {
                    int sdx = nx - prevX;
                    int sdy = ny - prevY;
                    if (!hasDir) {
                        dirX = sdx;
                        dirY = sdy;
                        hasDir = true;
                    } else if (sdx != dirX || sdy != dirY) {
                        emitTrack(net, curL, worldX(startX), worldY(startY), worldX(prevX), worldY(prevY));
                        startX = prevX;
                        startY = prevY;
                        dirX = sdx;
                        dirY = sdy;
                    }
                    prevX = nx;
                    prevY = ny;
                }
            }
            if (prevX != startX || prevY != startY) {
                emitTrack(net, curL, worldX(startX), worldY(startY), worldX(prevX), worldY(prevY));
            }
            // Stub from the grid to the exact target copper point (T-junction / pad / via)
            emitTrack(net, curL, worldX(prevX), worldY(prevY), br.ex, br.ey);
        }
    }

    std::string layerStyle = useEasyEdaNumeric ? "easyeda_numeric" : (useEasyEdaNames ? "easyeda" : "kicad");
    nlohmann::json warningsJson = nlohmann::json::array();
    for (const auto& w : warnings) warningsJson.push_back(w);

    outSuccess = (unroutedCount == 0 && drcViolations == 0);
    std::ostringstream summary;
    summary << "Routed " << routedCount << "/" << nets.size() << " nets • " << drcViolations
            << " DRC • " << ripupPassesUsed << " rip-up passes • " << bridgesCount
            << " island bridges • grid " << r4(gridMm) << " mm";
    outSummary = summary.str();
    nlohmann::json res = {
        {"ok", outSuccess},
        {"total_nets", static_cast<int>(nets.size())},
        {"routed_nets", routedCount},
        {"unrouted_nets", unroutedCount},
        {"unrouted_net_names", unroutedNames},
        {"already_connected_nets", alreadyConnected},
        {"islands_bridged", bridgesCount},
        {"padless_islands_ignored", padlessIslandsIgnored},
        {"drc_violations", drcViolations},
        {"ripup_passes_used", ripupPassesUsed},
        {"rerouted_nets_count", reroutedNetsCount},
        {"units", units},
        {"bare_coordinate_units", "mil"},
        {"layers", numLayers},
        {"layer_style", layerStyle},
        {"grid_mm", r4(gridMm)},
        {"requested_grid_mm", r4(requestedGridMm)},
        {"grid_coarsened", gridCoarsened},
        {"grid_cells", {{"x", gxMax}, {"y", gyMax}, {"per_layer", static_cast<int64_t>(planeCells)}}},
        {"canvas", {{"mode", canvasMode}, {"origin_x_mm", r4(originX)}, {"origin_y_mm", r4(originY)},
                    {"width_mm", r4(spanX)}, {"height_mm", r4(spanY)}}},
        {"warnings", warningsJson},
        {"tracks", tracksJson},
        {"vias", viasJson},
        {"summary", outSummary}
    };
    if (gpuKernelExecuted) {
        res["gpu_kernel_executed"] = true;
    }
    attachProvenance(res, "eda_calc_pcb_autoroute");
    return res;
}

nlohmann::json EdaAccelerators::calcElectricalSynth(const nlohmann::json& args,
                                                    bool& outSuccess,
                                                    std::string& outSummary) const {
    auto makeSuccess = [this](nlohmann::json res) -> nlohmann::json {
        attachProvenance(res, "eda_calc_electrical_synth");
        return res;
    };

    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "eda_calc_electrical_synth requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    std::string rawMode = args.value("mode", "divider");
    std::string mode = rawMode;
    if (mode == "voltage_divider") mode = "divider";
    else if (mode == "trace_width") mode = "ipc2152";
    else if (mode == "microstrip" || mode == "diff_pair") mode = "impedance";

    if (mode == "divider" || mode == "feedback" || mode == "rc_filter") {
        double vin = args.value("vin_v", 5.0);
        double voutTarget = args.value("vout_target_v", args.value("vout_v", 3.3));
        std::string series = args.value("series", args.value("r_series", "E24"));
        if (vin <= 0.0 || voutTarget <= 0.0 || voutTarget >= vin) {
            outSuccess = false;
            outSummary = "Invalid divider voltages (require 0 < vout_target_v < vin_v)";
            return {{"ok", false}, {"error", outSummary}};
        }

        const double* basePtr = nullptr;
        size_t baseLen = 0;
        getESeriesBase(series, _config.preferJlcpcbBasicESeries, basePtr, baseLen);
        std::vector<double> candidates;
        for (double decade : {1000.0, 10000.0, 100000.0}) {
            for (size_t i = 0; i < baseLen; ++i) {
                candidates.push_back(basePtr[i] * decade);
            }
        }

        double bestRTop = 10000.0;
        double bestRBot = 10000.0;
        double bestErr = std::numeric_limits<double>::max();
        for (double rt : candidates) {
            for (double rb : candidates) {
                double vActual = vin * (rb / (rt + rb));
                double err = std::fabs(vActual - voutTarget) / voutTarget;
                if (err < bestErr) {
                    bestErr = err;
                    bestRTop = rt;
                    bestRBot = rb;
                }
            }
        }

        double voutActual = vin * (bestRBot / (bestRTop + bestRBot));
        double tol = (series == "E96") ? 0.01 : 0.01;
        double voutMin = vin * ((bestRBot * (1.0 - tol)) / (bestRTop * (1.0 + tol) + bestRBot * (1.0 - tol)));
        double voutMax = vin * ((bestRBot * (1.0 + tol)) / (bestRTop * (1.0 - tol) + bestRBot * (1.0 + tol)));

        outSuccess = true;
        outSummary = "Synthesized " + series + " divider R_top=" + std::to_string(static_cast<int>(bestRTop)) +
                     "R, R_bot=" + std::to_string(static_cast<int>(bestRBot)) + "R (err " +
                     std::to_string(bestErr * 100.0).substr(0, 4) + "%)";
        return makeSuccess({
            {"ok", true},
            {"mode", mode},
            {"r_top_ohm", bestRTop},
            {"r_bottom_ohm", bestRBot},
            {"vout_target_v", voutTarget},
            {"vout_actual_v", voutActual},
            {"vout_min_v", voutMin},
            {"vout_max_v", voutMax},
            {"relative_error_pct", bestErr * 100.0},
            {"summary", outSummary}
        });
    }

    if (mode == "ipc2152") {
        double currentA = std::max(0.05, args.value("current_a", 1.0));
        double tempRiseC = std::max(5.0, args.value("temp_rise_c", args.value("max_temp_rise_c", 20.0)));
        double copperOz = std::max(0.5, args.value("copper_oz", args.value("copper_weight_oz", _config.defaultCopperWeightOz)));
        double lengthMm = std::max(1.0, args.value("length_mm", args.value("trace_length_mm", 25.0)));

        // IPC-2152 / IPC-2221 analytical cross-sectional area in sq mils: I = k * dT^0.44 * A^0.725
        double areaExtMils2 = std::pow(currentA / (0.048 * std::pow(tempRiseC, 0.44)), 1.0 / 0.725);
        double areaIntMils2 = std::pow(currentA / (0.024 * std::pow(tempRiseC, 0.44)), 1.0 / 0.725);
        double thicknessMils = copperOz * 1.378;
        double extWidthMm = (areaExtMils2 / thicknessMils) * 0.0254;
        double intWidthMm = (areaIntMils2 / thicknessMils) * 0.0254;

        double thicknessM = (thicknessMils * 0.0254) * 1e-3;
        double widthM = std::max(0.15e-3, extWidthMm * 1e-3);
        double resistanceOhm = 1.724e-8 * (lengthMm * 1e-3) / (widthM * thicknessM);
        double voltageDropV = currentA * resistanceOhm;
        int recommendedVias = std::max(1, static_cast<int>(std::ceil(currentA / 1.2)));

        outSuccess = true;
        outSummary = "IPC-2152 trace width " + std::to_string(extWidthMm).substr(0, 4) + " mm (" +
                     std::to_string(recommendedVias) + " vias) for " + std::to_string(currentA).substr(0, 4) + " A";
        return makeSuccess({
            {"ok", true},
            {"mode", "ipc2152"},
            {"external_width_mm", extWidthMm},
            {"internal_width_mm", intWidthMm},
            {"resistance_ohm", resistanceOhm},
            {"voltage_drop_v", voltageDropV},
            {"recommended_vias", recommendedVias},
            {"summary", outSummary}
        });
    }

    if (mode == "impedance") {
        std::string type = args.value("type", (rawMode == "diff_pair") ? "diff_microstrip" : "microstrip");
        if (type == "diff_pair") type = "diff_microstrip";
        double targetZ = std::max(20.0, args.value("target_z_ohm",
                                        args.value("z0_target_ohm",
                                        args.value("z_diff_target_ohm", (type == "diff_microstrip") ? 90.0 : 50.0))));
        double hMm = std::max(0.05, args.value("dielectric_h_mm", args.value("substrate_height_mm", 0.15)));
        double er = std::max(2.0, args.value("er", 4.4));
        double sMm = std::max(0.08, args.value("spacing_mm", 0.15));
        double tMm = 0.035;

        // Binary search width wMm for Hammerstad-Jensen microstrip impedance
        auto calcZ0 = [&](double w) {
            double effEr = (er + 1.0) * 0.5 + ((er - 1.0) * 0.5) / std::sqrt(1.0 + 12.0 * hMm / w);
            double z0 = (87.0 / std::sqrt(effEr + 1.41)) * std::log((5.98 * hMm) / (0.8 * w + tMm));
            if (type == "diff_microstrip") {
                return 2.0 * z0 * (1.0 - 0.48 * std::exp(-0.96 * (sMm / hMm)));
            }
            return z0;
        };

        double lo = 0.04, hi = 4.0;
        for (int i = 0; i < 40; ++i) {
            double mid = 0.5 * (lo + hi);
            if (calcZ0(mid) > targetZ) {
                lo = mid;
            } else {
                hi = mid;
            }
        }
        double bestW = 0.5 * (lo + hi);
        double achievedZ = calcZ0(bestW);

        outSuccess = true;
        outSummary = "Solved " + type + " width=" + std::to_string(bestW).substr(0, 5) +
                     " mm for Z=" + std::to_string(static_cast<int>(targetZ)) + " Ohm";
        return makeSuccess({
            {"ok", true},
            {"mode", "impedance"},
            {"type", type},
            {"trace_width_mm", bestW},
            {"spacing_mm", sMm},
            {"achieved_z_ohm", achievedZ},
            {"summary", outSummary}
        });
    }

    if (mode == "power_budget") {
        double totalPowerW = 0.0;
        nlohmann::json bom = nlohmann::json::array();
        if (args.contains("rails") && args["rails"].is_array()) {
            for (const auto& r : args["rails"]) {
                if (!r.is_object()) continue;
                std::string rName = r.value("name", "VCC");
                double v = r.value("voltage_v", r.value("voltage", r.value("v", 3.3)));
                double iA = r.contains("current_a")
                    ? r.value("current_a", 0.5)
                    : (r.contains("max_current_a")
                        ? r.value("max_current_a", 0.5)
                        : (r.contains("current_ma") ? (r.value("current_ma", 500.0) / 1000.0) : 0.5));
                int pins = std::max(1, r.value("ic_power_pins", 2));
                totalPowerW += v * iA;
                bom.push_back({
                    {"rail", rName},
                    {"hf_100nf_count", pins},
                    {"bulk_10uf_count", std::max(1, static_cast<int>(std::ceil(iA / 0.8)))},
                    {"voltage_rating_v", std::max(10.0, std::ceil(v * 2.0))}
                });
            }
        }
        outSuccess = true;
        outSummary = "Synthesized power & decoupling budget (" +
                     std::to_string(totalPowerW).substr(0, 5) + " W total)";
        return makeSuccess({
            {"ok", true},
            {"mode", "power_budget"},
            {"total_power_w", totalPowerW},
            {"decoupling_bom", bom},
            {"rails", bom},
            {"summary", outSummary}
        });
    }

    outSuccess = false;
    outSummary = "Unsupported electrical synth mode: " + mode;
    return {{"ok", false}, {"error", outSummary}};
}

nlohmann::json EdaAccelerators::calcRobotKinematicsStatics(const nlohmann::json& args,
                                                           bool& outSuccess,
                                                           std::string& outSummary) const {
    if (!args.is_object() || !args.contains("links") || !args["links"].is_array() ||
        args["links"].empty()) {
        outSuccess = false;
        outSummary = "cad_calc_robot_kinematics_statics requires non-empty links[] array";
        return {{"ok", false}, {"error", outSummary}};
    }

    struct LinkSpec {
        std::string name;
        double lengthM = 0.1;
        double massKg = 0.1;
        double angleDeg = 0.0;
    };

    std::vector<LinkSpec> links;
    for (const auto& l : args["links"]) {
        if (!l.is_object()) continue;
        LinkSpec ls;
        ls.name = l.value("name", "link_" + std::to_string(links.size() + 1));
        double lenM = l.contains("length_m")
            ? l.value("length_m", 0.10)
            : (l.contains("length_mm") ? (l.value("length_mm", 100.0) / 1000.0) : 0.10);
        double mKg = l.contains("mass_kg")
            ? l.value("mass_kg", 0.10)
            : (l.contains("mass_g") ? (l.value("mass_g", 100.0) / 1000.0) : 0.10);
        ls.lengthM = std::max(0.005, lenM);
        ls.massKg = std::max(0.001, mKg);
        ls.angleDeg = l.value("angle_deg", l.value("joint_angle_deg", 0.0));
        links.push_back(ls);
    }

    double payloadKg = args.contains("payload_kg")
        ? std::max(0.0, args.value("payload_kg", 0.10))
        : (args.contains("payload_g") ? std::max(0.0, args.value("payload_g", 100.0) / 1000.0) : 0.10);
    double dynFactor = std::max(1.0, args.value("dynamic_factor", 1.35));
    double stallTorqueKgCm = std::max(0.5, args.value("servo_stall_torque_kgcm", 20.0));
    double stallTorqueNm = stallTorqueKgCm * 0.0980665;
    double polyHalfWidthM = std::max(0.01, args.value("support_polygon_half_width_m", 0.08));

    // Forward kinematics in planar XY (and full horizontal worst-case lever arm)
    double curX = 0.0, curY = 0.0, cumAngleRad = 0.0;
    double totalMassKg = payloadKg;
    double comMomentX = 0.0;

    std::vector<double> jointX(links.size(), 0.0);
    std::vector<double> linkComX(links.size(), 0.0);

    for (size_t i = 0; i < links.size(); ++i) {
        jointX[i] = curX;
        cumAngleRad += links[i].angleDeg * (kPi / 180.0);
        double nx = curX + links[i].lengthM * std::cos(cumAngleRad);
        double ny = curY + links[i].lengthM * std::sin(cumAngleRad);
        linkComX[i] = 0.5 * (curX + nx);
        totalMassKg += links[i].massKg;
        comMomentX += links[i].massKg * linkComX[i];
        curX = nx;
        curY = ny;
    }
    comMomentX += payloadKg * curX;
    double wholeBodyComX = (totalMassKg > 0.0) ? (comMomentX / totalMassKg) : 0.0;
    double supportMarginM = polyHalfWidthM - std::fabs(wholeBodyComX);

    nlohmann::json jointsJson = nlohmann::json::array();
    double minSafetyFactor = std::numeric_limits<double>::max();

    for (size_t i = 0; i < links.size(); ++i) {
        // Worst-case horizontal outreach gravity moment from joint i outward
        double distAccumM = 0.0;
        double staticMomentNm = 0.0;
        for (size_t j = i; j < links.size(); ++j) {
            double comArm = distAccumM + 0.5 * links[j].lengthM;
            staticMomentNm += links[j].massKg * kGravity * comArm;
            distAccumM += links[j].lengthM;
        }
        staticMomentNm += payloadKg * kGravity * distAccumM;
        double reqTorqueNm = staticMomentNm * dynFactor;
        double reqTorqueKgCm = reqTorqueNm / 0.0980665;
        double safetyFactor = stallTorqueNm / std::max(1e-6, reqTorqueNm);
        minSafetyFactor = std::min(minSafetyFactor, safetyFactor);

        // Torsional shaft sizing: d = (16 * T / (pi * tau_allow))^(1/3), tau_allow = 45 MPa
        double minShaftDiaMm = std::max(3.0, std::cbrt((16.0 * reqTorqueNm) / (kPi * 45e6)) * 1000.0);

        jointsJson.push_back({
            {"joint_index", i},
            {"link_name", links[i].name},
            {"static_torque_nm", staticMomentNm},
            {"required_torque_nm", reqTorqueNm},
            {"required_torque_kgcm", reqTorqueKgCm},
            {"safety_factor", safetyFactor},
            {"min_shaft_diameter_mm", minShaftDiaMm}
        });
    }

    double reachM = std::hypot(curX, curY);
    outSuccess = true;
    outSummary = "Computed FK/statics for " + std::to_string(links.size()) +
                 " joints • Min servo SF=" + std::to_string(minSafetyFactor).substr(0, 4);
    nlohmann::json res = {
        {"ok", true},
        {"joints", jointsJson},
        {"end_effector", {
            {"x_m", curX},
            {"y_m", curY},
            {"reach_m", reachM}
        }},
        {"whole_body_com_x_m", wholeBodyComX},
        {"support_polygon_margin_m", supportMarginM},
        {"min_servo_safety_factor", minSafetyFactor},
        {"summary", outSummary}
    };
    attachProvenance(res, "cad_calc_robot_kinematics_statics");
    return res;
}

nlohmann::json EdaAccelerators::calcMechanismSynth(const nlohmann::json& args,
                                                   bool& outSuccess,
                                                   std::string& outSummary) const {
    auto makeSuccess = [this](nlohmann::json res) -> nlohmann::json {
        attachProvenance(res, "cad_calc_mechanism_synth");
        return res;
    };

    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "cad_calc_mechanism_synth requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    std::string type = args.value("type", args.value("mechanism_type", "spur_gear"));
    if (type == "four_bar_linkage") type = "four_bar";
    else if (type == "planetary_gear") type = "planetary";
    double clearanceMm = std::max(0.02, args.value("clearance_mm", 0.20));
    double pressureAngleDeg = args.value("pressure_angle_deg", _config.defaultGearPressureAngleDeg);
    double alphaRad = pressureAngleDeg * (kPi / 180.0);

    if (type == "spur_gear" || type == "planetary") {
        double modMm = std::max(0.3, args.value("module_mm", 1.0));
        int zSun = args.value("sun_teeth", args.value("teeth_1", args.value("teeth", 16)));
        int zPlanet = args.value("planet_teeth", args.value("teeth_2", args.value("teeth", 16)));
        double thicknessMm = std::max(1.0, args.value("thickness_mm", 6.0));

        if (zSun < 6 || zPlanet < 6) {
            outSuccess = false;
            outSummary = "Gear tooth counts must be >= 6";
            return {{"ok", false}, {"error", outSummary}};
        }

        int zRing = zSun + 2 * zPlanet;
        if (type == "planetary") {
            if (args.contains("ring_teeth") && args["ring_teeth"].is_number_integer()) {
                int reqRing = args["ring_teeth"].get<int>();
                if (reqRing != zRing) {
                    outSuccess = false;
                    outSummary = "Planetary gear assembly constraint violated: Z_s + 2*Z_p != Z_r";
                    return {{"ok", false}, {"error", outSummary}};
                }
            }
        }

        double pitchRadiusSun = 0.5 * modMm * zSun;
        double baseRadiusSun = pitchRadiusSun * std::cos(alphaRad);
        double outerRadiusSun = pitchRadiusSun + modMm - 0.5 * clearanceMm;
        double rootRadiusSun = std::max(1.0, pitchRadiusSun - 1.25 * modMm - 0.5 * clearanceMm);
        double centerDistMm = 0.5 * modMm * (zSun + zPlanet) + clearanceMm;
        double carrierRatio = 1.0 + static_cast<double>(zRing) / static_cast<double>(zSun);

        // Emit involute tooth profile coordinates
        nlohmann::json profilePts = nlohmann::json::array();
        for (int i = 0; i <= 10; ++i) {
            double r = std::max(baseRadiusSun, rootRadiusSun) +
                       (outerRadiusSun - std::max(baseRadiusSun, rootRadiusSun)) * (i / 10.0);
            double phi = std::acos(std::clamp(baseRadiusSun / r, -1.0, 1.0));
            double invPhi = std::tan(phi) - phi;
            profilePts.push_back({
                {"x_mm", r * std::cos(invPhi)},
                {"y_mm", r * std::sin(invPhi)}
            });
        }

        std::ostringstream py;
        py << "import FreeCAD, Part\n"
           << "doc = FreeCAD.ActiveDocument or FreeCAD.newDocument('Mechanism')\n"
           << "sun = Part.makeCylinder(" << outerRadiusSun << ", " << thicknessMm << ")\n"
           << "bore = Part.makeCylinder(" << (2.5 + 0.5 * clearanceMm) << ", " << thicknessMm << ")\n"
           << "sun_body = doc.addObject('Part::Feature', 'SunGear')\n"
           << "sun_body.Shape = sun.cut(bore)\n"
           << "doc.recompute()\n";

        outSuccess = true;
        outSummary = "Synthesized " + type + " (m=" + std::to_string(modMm).substr(0, 4) +
                     " mm, center_dist=" + std::to_string(centerDistMm).substr(0, 5) + " mm)";
        return makeSuccess({
            {"ok", true},
            {"type", type},
            {"module_mm", modMm},
            {"sun_teeth", zSun},
            {"planet_teeth", zPlanet},
            {"ring_teeth", zRing},
            {"center_distance_mm", centerDistMm},
            {"carrier_reduction_ratio", carrierRatio},
            {"pitch_diameter_sun_mm", 2.0 * pitchRadiusSun},
            {"outer_diameter_sun_mm", 2.0 * outerRadiusSun},
            {"root_diameter_sun_mm", 2.0 * rootRadiusSun},
            {"profile_points", profilePts},
            {"freecad_python", py.str()},
            {"summary", outSummary}
        });
    }

    if (type == "servo_spline" || type == "timing_pulley") {
        std::string stdName = args.value("spline_standard", "25T_futaba");
        int teeth = (stdName.find("24T") != std::string::npos) ? 24 : args.value("teeth", 25);
        double outerDiaMm = (teeth == 24) ? 5.80 : 5.92;
        double rootDiaMm = outerDiaMm - 0.60;
        double rOuter = 0.5 * outerDiaMm + 0.5 * clearanceMm;
        double rRoot = 0.5 * rootDiaMm + 0.5 * clearanceMm;

        nlohmann::json pts = nlohmann::json::array();
        for (int i = 0; i < teeth * 2; ++i) {
            double ang = (kPi * i) / teeth;
            double r = (i % 2 == 0) ? rOuter : rRoot;
            pts.push_back({{"x_mm", r * std::cos(ang)}, {"y_mm", r * std::sin(ang)}});
        }

        outSuccess = true;
        outSummary = "Synthesized " + std::to_string(teeth) + "T spline/pulley profile with " +
                     std::to_string(clearanceMm).substr(0, 4) + " mm 3D-print clearance";
        return makeSuccess({
            {"ok", true},
            {"type", type},
            {"tooth_count", teeth},
            {"outer_diameter_mm", 2.0 * rOuter},
            {"root_diameter_mm", 2.0 * rRoot},
            {"profile_points", pts},
            {"summary", outSummary}
        });
    }

    if (type == "four_bar") {
        double g = args.value("ground_mm", args.value("ground_length_mm", 40.0));
        double a = args.value("crank_mm", args.value("crank_length_mm", 15.0));
        double b = args.value("coupler_mm", args.value("coupler_length_mm", 35.0));
        double c = args.value("rocker_mm", args.value("rocker_length_mm", 30.0));
        std::vector<double> lengths = {g, a, b, c};
        std::sort(lengths.begin(), lengths.end());
        double s = lengths[0];
        double l = lengths[3];
        double p = lengths[1];
        double q = lengths[2];
        bool isGrashof = (s + l <= p + q) && (a == s);

        double cosMin = std::clamp((b * b + c * c - (g - a) * (g - a)) / (2.0 * b * c), -1.0, 1.0);
        double cosMax = std::clamp((b * b + c * c - (g + a) * (g + a)) / (2.0 * b * c), -1.0, 1.0);
        double minTransDeg = std::acos(cosMin) * (180.0 / kPi);
        double maxTransDeg = std::acos(cosMax) * (180.0 / kPi);

        outSuccess = isGrashof;
        outSummary = isGrashof ? "Grashof crank-rocker 4-bar linkage verified"
                               : "Non-Grashof 4-bar linkage rejected (crank cannot rotate 360 deg)";
        nlohmann::json fourBarRes = {
            {"ok", isGrashof},
            {"type", "four_bar"},
            {"is_grashof", isGrashof},
            {"min_transmission_angle_deg", minTransDeg},
            {"max_transmission_angle_deg", maxTransDeg},
            {"summary", outSummary}
        };
        if (isGrashof) {
            return makeSuccess(fourBarRes);
        }
        return fourBarRes;
    }

    outSuccess = false;
    outSummary = "Unknown mechanism type: " + type;
    return {{"ok", false}, {"error", outSummary}};
}

nlohmann::json EdaAccelerators::calcPrintOrientationDfm(const nlohmann::json& args,
                                                        bool& outSuccess,
                                                        std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "cad_calc_print_orientation_dfm requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    std::string mat = args.value("material", _config.defaultFilamentMaterial);
    double sigmaXyMpa = 45.0;
    double sigmaZMpa = 22.0;
    if (mat == "PLA") {
        sigmaXyMpa = 55.0;
        sigmaZMpa = 26.0;
    } else if (mat == "ABS" || mat == "ASA") {
        sigmaXyMpa = 40.0;
        sigmaZMpa = 18.0;
    } else if (mat == "PA-CF") {
        sigmaXyMpa = 85.0;
        sigmaZMpa = 38.0;
    }

    double defaultAreaMm2 = 120.0;
    double defaultSecModMm3 = 250.0;
    if (args.contains("bbox_mm")) {
        double bx = 20.0, by = 10.0;
        if (args["bbox_mm"].is_array() && args["bbox_mm"].size() >= 2 &&
            args["bbox_mm"][0].is_number() && args["bbox_mm"][1].is_number()) {
            bx = std::max(2.0, args["bbox_mm"][0].get<double>());
            by = std::max(2.0, args["bbox_mm"][1].get<double>());
        } else if (args["bbox_mm"].is_object()) {
            bx = std::max(2.0, args["bbox_mm"].value("x", 20.0));
            by = std::max(2.0, args["bbox_mm"].value("y", 10.0));
        }
        defaultAreaMm2 = std::max(10.0, bx * by * 0.25);
        defaultSecModMm3 = std::max(20.0, (bx * by * by) / 12.0);
    }
    double areaMm2 = std::max(10.0, args.value("cross_section_mm2", defaultAreaMm2));
    double secModMm3 = std::max(20.0, args.value("section_modulus_mm3", defaultSecModMm3));

    double fx = 0.0, fy = 0.0, fz = 50.0;
    if (args.contains("load_vector_n") && args["load_vector_n"].is_object()) {
        fx = args["load_vector_n"].value("fx", 0.0);
        fy = args["load_vector_n"].value("fy", 0.0);
        fz = args["load_vector_n"].value("fz", 50.0);
    } else if (args.contains("load_cases") && args["load_cases"].is_array() && !args["load_cases"].empty()) {
        const auto& lc = args["load_cases"][0];
        if (lc.is_object()) {
            if (lc.contains("force_n") && lc["force_n"].is_array() && lc["force_n"].size() >= 3) {
                fx = lc["force_n"][0].is_number() ? lc["force_n"][0].get<double>() : 0.0;
                fy = lc["force_n"][1].is_number() ? lc["force_n"][1].get<double>() : 0.0;
                fz = lc["force_n"][2].is_number() ? lc["force_n"][2].get<double>() : 50.0;
            } else {
                fx = lc.value("fx", 0.0);
                fy = lc.value("fy", 0.0);
                fz = lc.value("fz", 50.0);
            }
        }
    }
    double mx = 1.0, my = 0.0, mz = 0.0;
    if (args.contains("moment_vector_nm") && args["moment_vector_nm"].is_object()) {
        mx = args["moment_vector_nm"].value("mx", 1.0);
        my = args["moment_vector_nm"].value("my", 0.0);
        mz = args["moment_vector_nm"].value("mz", 0.0);
    }
    double hx = 0.0, hy = 0.0, hz = 1.0;
    if (args.contains("bearing_bore_axis") && args["bearing_bore_axis"].is_object()) {
        hx = args["bearing_bore_axis"].value("x", 0.0);
        hy = args["bearing_bore_axis"].value("y", 0.0);
        hz = args["bearing_bore_axis"].value("z", 1.0);
    }

    double fMag = std::max(1.0, std::sqrt(fx * fx + fy * fy + fz * fz));
    double mMagNmm = std::sqrt(mx * mx + my * my + mz * mz) * 1000.0;

    // Principal tensile direction unit vector (combination of force and bending normal)
    double px = fx + my * 100.0;
    double py = fy - mx * 100.0;
    double pz = fz;
    double pNorm = std::max(1e-6, std::sqrt(px * px + py * py + pz * pz));
    px /= pNorm;
    py /= pNorm;
    pz /= pNorm;

    double hNorm = std::max(1e-6, std::sqrt(hx * hx + hy * hy + hz * hz));
    hx /= hNorm;
    hy /= hNorm;
    hz /= hNorm;

    struct CandidateOrientation {
        std::string label;
        double rxDeg;
        double ryDeg;
        double rzDeg;
        double nx, ny, nz; // Slicer Z-build normal in part frame
    };

    const std::vector<CandidateOrientation> candidates = {
        {"flat_xy", 0.0, 0.0, 0.0, 0.0, 0.0, 1.0},
        {"on_side_x90", 90.0, 0.0, 0.0, 0.0, 1.0, 0.0},
        {"standing_y90", 0.0, 90.0, 0.0, 1.0, 0.0, 0.0},
        {"tilted_45_x", 45.0, 0.0, 0.0, 0.0, 0.7071, 0.7071},
        {"tilted_45_y", 0.0, 45.0, 0.0, 0.7071, 0.0, 0.7071},
        {"inverted_z180", 180.0, 0.0, 0.0, 0.0, 0.0, -1.0}
    };

    nlohmann::json candArray = nlohmann::json::array();
    double bestScore = -1e9;
    nlohmann::json bestOrientation;
    double bestDelamSf = 1.0;

    bool gpuKernelExecuted = false;
    if (_capabilities.gpuAvailable && OpenClEngine::instance().isGpuAvailable()) {
        std::vector<float> candNormals;
        for (const auto& c : candidates) {
            double supportPenalty = (c.rxDeg == 45.0 || c.ryDeg == 45.0) ? 0.25 : 0.05;
            candNormals.push_back(static_cast<float>(c.nx));
            candNormals.push_back(static_cast<float>(c.ny));
            candNormals.push_back(static_cast<float>(c.nz));
            candNormals.push_back(static_cast<float>(supportPenalty));
        }
        std::vector<float> outMetrics;
        if (OpenClEngine::instance().runOrientationOverhangs(
                static_cast<int>(candidates.size()),
                static_cast<float>(px), static_cast<float>(py), static_cast<float>(pz),
                static_cast<float>(hx), static_cast<float>(hy), static_cast<float>(hz),
                static_cast<float>(fMag), static_cast<float>(areaMm2),
                static_cast<float>(mMagNmm), static_cast<float>(secModMm3),
                static_cast<float>(sigmaZMpa), static_cast<float>(sigmaXyMpa),
                candNormals, outMetrics)) {
            gpuKernelExecuted = true;
            candArray = nlohmann::json::array();
            bestScore = -1e9;
            for (size_t ci = 0; ci < candidates.size(); ++ci) {
                const auto& c = candidates[ci];
                double delamSf = outMetrics[ci * 4 + 0];
                double fiberSf = outMetrics[ci * 4 + 1];
                double boreCircularity = outMetrics[ci * 4 + 2];
                double score = outMetrics[ci * 4 + 3];
                nlohmann::json entry = {
                    {"label", c.label},
                    {"rx_deg", c.rxDeg},
                    {"ry_deg", c.ryDeg},
                    {"rz_deg", c.rzDeg},
                    {"delamination_safety_factor", delamSf},
                    {"in_plane_safety_factor", fiberSf},
                    {"bore_circularity_index", boreCircularity},
                    {"score", score}
                };
                candArray.push_back(entry);
                if (score > bestScore) {
                    bestScore = score;
                    bestOrientation = entry;
                    bestDelamSf = delamSf;
                }
            }
        }
    }

    if (!gpuKernelExecuted) {
        for (const auto& c : candidates) {
            double zAlign = std::fabs(px * c.nx + py * c.ny + pz * c.nz);
            double xyAlign = std::sqrt(std::max(0.0, 1.0 - zAlign * zAlign));
            double normalStressMpa = (fMag / areaMm2) + (mMagNmm / secModMm3);
            double zInterlayerStressMpa = std::max(0.1, normalStressMpa * (0.25 + 0.75 * zAlign));
            double xyFiberStressMpa = std::max(0.1, normalStressMpa * xyAlign);

            double delamSf = sigmaZMpa / zInterlayerStressMpa;
            double fiberSf = sigmaXyMpa / xyFiberStressMpa;
            double boreCircularity = std::fabs(hx * c.nx + hy * c.ny + hz * c.nz);
            double supportPenalty = (c.rxDeg == 45.0 || c.ryDeg == 45.0) ? 0.25 : 0.05;

            double score = delamSf * 2.0 + boreCircularity * 1.5 - supportPenalty;
            nlohmann::json entry = {
                {"label", c.label},
                {"rx_deg", c.rxDeg},
                {"ry_deg", c.ryDeg},
                {"rz_deg", c.rzDeg},
                {"delamination_safety_factor", delamSf},
                {"in_plane_safety_factor", fiberSf},
                {"bore_circularity_index", boreCircularity},
                {"score", score}
            };
            candArray.push_back(entry);
            if (score > bestScore) {
                bestScore = score;
                bestOrientation = entry;
                bestDelamSf = delamSf;
            }
        }
    }

    outSuccess = true;
    outSummary = "Optimal FDM orientation " + bestOrientation.value("label", "on_side_x90") +
                 " (" + mat + " interlayer SF=" + std::to_string(bestDelamSf).substr(0, 4) + ")";
    nlohmann::json res = {
        {"ok", true},
        {"material", mat},
        {"optimal_orientation", bestOrientation},
        {"delamination_safety_factor", bestDelamSf},
        {"candidates", candArray},
        {"summary", outSummary}
    };
    if (gpuKernelExecuted) {
        res["gpu_kernel_executed"] = true;
    }
    attachProvenance(res, "cad_calc_print_orientation_dfm");
    return res;
}

nlohmann::json EdaAccelerators::calcPcbConnectivityAudit(const nlohmann::json& args,
                                                         bool& outSuccess,
                                                         std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "eda_calc_pcb_connectivity_audit requires JSON object arguments";
        return {{"ok", false}, {"error", outSummary}};
    }

    auto canonicalLayer = [](const nlohmann::json& obj, const std::string& defVal) -> std::string {
        std::string raw = defVal;
        if (obj.contains("layer")) {
            if (obj["layer"].is_string()) {
                raw = obj["layer"].get<std::string>();
            } else if (obj["layer"].is_number_integer()) {
                raw = std::to_string(obj["layer"].get<int>());
            }
        }
        if (raw == "TopLayer" || raw == "1" || raw == "F.Cu" || raw == "Top") return "TopLayer";
        if (raw == "BottomLayer" || raw == "2" || raw == "B.Cu" || raw == "Bottom") return "BottomLayer";
        if (raw == "InnerLayer1" || raw == "15" || raw == "In1.Cu") return "InnerLayer1";
        if (raw == "InnerLayer2" || raw == "16" || raw == "In2.Cu") return "InnerLayer2";
        if (raw == "InnerLayer3" || raw == "17" || raw == "In3.Cu") return "InnerLayer3";
        if (raw == "InnerLayer4" || raw == "18" || raw == "In4.Cu") return "InnerLayer4";
        if (raw == "MultiLayer" || raw == "12" || raw == "*.Cu" ||
            raw == "Multi-Layer" || raw == "PTH" || raw == "thru_hole") {
            return "MultiLayer";
        }
        return raw;
    };

    auto layersMatch = [](const std::string& l1, const std::string& l2) -> bool {
        return l1 == l2 || l1 == "MultiLayer" || l2 == "MultiLayer";
    };

    auto distPointToSeg = [](double px, double py,
                             double x1, double y1,
                             double x2, double y2) -> double {
        double dx = x2 - x1;
        double dy = y2 - y1;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
            return std::hypot(px - x1, py - y1);
        }
        double t = std::clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0.0, 1.0);
        return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
    };

    auto distSegToSeg = [&](double ax1, double ay1, double ax2, double ay2,
                            double bx1, double by1, double bx2, double by2) -> double {
        double d1 = distPointToSeg(ax1, ay1, bx1, by1, bx2, by2);
        double d2 = distPointToSeg(ax2, ay2, bx1, by1, bx2, by2);
        double d3 = distPointToSeg(bx1, by1, ax1, ay1, ax2, ay2);
        double d4 = distPointToSeg(bx2, by2, ax1, ay1, ax2, ay2);
        return std::min({d1, d2, d3, d4});
    };

    struct AuditPad {
        std::string id;
        std::string ref;
        std::string pad;
        std::string net;
        double x = 0.0;
        double y = 0.0;
        double sx = 0.6;
        double sy = 0.6;
        double cosR = 1.0;
        double sinR = 0.0;
        std::string layer;
    };
    auto distPointToPad = [](double wx, double wy, const AuditPad& pad) -> double {
        double dx = wx - pad.x;
        double dy = wy - pad.y;
        double lx = dx * pad.cosR + dy * pad.sinR;
        double ly = -dx * pad.sinR + dy * pad.cosR;
        return pointRectDistanceLocal(lx, ly, 0.5 * pad.sx, 0.5 * pad.sy);
    };
    auto distSegToPad = [](double x1, double y1, double x2, double y2, const AuditPad& pad) -> double {
        double dx1 = x1 - pad.x, dy1 = y1 - pad.y;
        double dx2 = x2 - pad.x, dy2 = y2 - pad.y;
        double lx1 = dx1 * pad.cosR + dy1 * pad.sinR;
        double ly1 = -dx1 * pad.sinR + dy1 * pad.cosR;
        double lx2 = dx2 * pad.cosR + dy2 * pad.sinR;
        double ly2 = -dx2 * pad.sinR + dy2 * pad.cosR;
        return segmentRectDistanceLocal(lx1, ly1, lx2, ly2, 0.5 * pad.sx, 0.5 * pad.sy);
    };
    auto padsTouch = [&](const AuditPad& a, const AuditPad& b, double tol) -> bool {
        const double hwa = 0.5 * a.sx, hha = 0.5 * a.sy;
        const double hwb = 0.5 * b.sx, hhb = 0.5 * b.sy;
        const double localPtsA[5][2] = {{0.0, 0.0}, {-hwa, -hha}, {hwa, -hha}, {hwa, hha}, {-hwa, hha}};
        for (const auto& lp : localPtsA) {
            double wx = a.x + lp[0] * a.cosR - lp[1] * a.sinR;
            double wy = a.y + lp[0] * a.sinR + lp[1] * a.cosR;
            if (distPointToPad(wx, wy, b) <= tol) return true;
        }
        const double localPtsB[5][2] = {{0.0, 0.0}, {-hwb, -hhb}, {hwb, -hhb}, {hwb, hhb}, {-hwb, hhb}};
        for (const auto& lp : localPtsB) {
            double wx = b.x + lp[0] * b.cosR - lp[1] * b.sinR;
            double wy = b.y + lp[0] * b.sinR + lp[1] * b.cosR;
            if (distPointToPad(wx, wy, a) <= tol) return true;
        }
        return false;
    };
    struct AuditTrack {
        std::string id;
        std::string net;
        std::string layer;
        double x1 = 0.0;
        double y1 = 0.0;
        double x2 = 0.0;
        double y2 = 0.0;
        double width = 0.25;
    };
    struct AuditVia {
        std::string id;
        std::string net;
        double x = 0.0;
        double y = 0.0;
        double diameter = 0.6;
        double drill = 0.3;
    };
    struct AuditPour {
        std::string id;
        std::string net;
        std::string layer;
        std::vector<std::pair<double, double>> poly;
        bool hasBbox = false;
        double xMin = 0.0;
        double yMin = 0.0;
        double xMax = 0.0;
        double yMax = 0.0;
    };

    std::string units = args.value("units", "");
    bool isMilUnits = (units == "mil");
    if (units.empty() && args.contains("components") && !args.contains("footprints") &&
        !args.contains("pcb_components")) {
        if (args.contains("pads") && args["pads"].is_array()) {
            for (const auto& p : args["pads"]) {
                if (p.is_object() && (std::fabs(p.value("x", 0.0)) > 300.0 ||
                                      std::fabs(p.value("y", 0.0)) > 300.0)) {
                    isMilUnits = true;
                    break;
                }
            }
        }
    }

    const double mmToCoord = isMilUnits ? (1.0 / 0.0254) : 1.0;
    const double defaultPadSize = 0.6 * mmToCoord;
    const double minPadSize = 0.1 * mmToCoord;
    const double defaultTrackWidth = 0.25 * mmToCoord;
    const double minTrackWidth = 0.05 * mmToCoord;
    const double defaultViaDia = 0.6 * mmToCoord;
    const double minViaDia = 0.2 * mmToCoord;
    const double defaultViaDrill = 0.3 * mmToCoord;
    const double minViaDrill = 0.1 * mmToCoord;
    const double touchTol = 0.08 * mmToCoord;

    std::vector<AuditPad> allPads;
    std::set<std::string> pcbRefs;

    auto extractPositiveNumber = [](const nlohmann::json& obj, const char* key) -> double {
        if (obj.contains(key) && obj[key].is_number()) {
            double v = obj[key].get<double>();
            if (v > 0.0) return v;
        }
        return 0.0;
    };

    auto ingestPadObj = [&](const nlohmann::json& p, const std::string& parentRef) {
        if (!p.is_object()) return;
        AuditPad ap;
        ap.id = p.value("id", "pad_" + std::to_string(allPads.size() + 1));
        ap.ref = p.value("ref", p.value("reference", parentRef));
        if (p.contains("number") && p["number"].is_string()) {
            ap.pad = p["number"].get<std::string>();
        } else if (p.contains("number") && p["number"].is_number_integer()) {
            ap.pad = std::to_string(p["number"].get<int64_t>());
        } else if (p.contains("pad") && p["pad"].is_string()) {
            ap.pad = p["pad"].get<std::string>();
        } else if (p.contains("pad") && p["pad"].is_number_integer()) {
            ap.pad = std::to_string(p["pad"].get<int64_t>());
        }
        ap.net = p.value("net", "");
        ap.x = p.value("x", p.value("x_mm", 0.0));
        ap.y = p.value("y", p.value("y_mm", 0.0));

        double rawSx = extractPositiveNumber(p, "size_x");
        if (rawSx <= 0.0) rawSx = extractPositiveNumber(p, "width");
        if (rawSx <= 0.0) {
            double wMm = extractPositiveNumber(p, "width_mm");
            if (wMm > 0.0) rawSx = wMm * mmToCoord;
        }
        double rawSy = extractPositiveNumber(p, "size_y");
        if (rawSy <= 0.0) rawSy = extractPositiveNumber(p, "height");
        if (rawSy <= 0.0) {
            double hMm = extractPositiveNumber(p, "height_mm");
            if (hMm > 0.0) rawSy = hMm * mmToCoord;
        }
        if ((rawSx <= 0.0 || rawSy <= 0.0) && p.contains("pad") && p["pad"].is_array() &&
            !p["pad"].empty() && p["pad"][0].is_string()) {
            std::string shape = p["pad"][0].get<std::string>();
            for (char& c : shape) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if ((shape == "OVAL" || shape == "RECT" || shape == "ELLIPSE" || shape == "OCTAGON") &&
                p["pad"].size() >= 3 && p["pad"][1].is_number() && p["pad"][2].is_number()) {
                if (rawSx <= 0.0) rawSx = p["pad"][1].get<double>();
                if (rawSy <= 0.0) rawSy = p["pad"][2].get<double>();
            } else if ((shape == "ROUND" || shape == "CIRCLE") &&
                       p["pad"].size() >= 2 && p["pad"][1].is_number()) {
                double d = p["pad"][1].get<double>();
                if (rawSx <= 0.0) rawSx = d;
                if (rawSy <= 0.0) rawSy = d;
            }
        }
        if ((rawSx <= 0.0 || rawSy <= 0.0) && p.contains("polygon") && p["polygon"].is_array() &&
            p["polygon"].size() >= 4 && p["polygon"][0].is_number()) {
            double minX = 1e18, maxX = -1e18, minY = 1e18, maxY = -1e18;
            for (size_t k = 0; k + 1 < p["polygon"].size(); k += 2) {
                if (!p["polygon"][k].is_number() || !p["polygon"][k + 1].is_number()) continue;
                double px = p["polygon"][k].get<double>();
                double py = p["polygon"][k + 1].get<double>();
                minX = std::min(minX, px);
                maxX = std::max(maxX, px);
                minY = std::min(minY, py);
                maxY = std::max(maxY, py);
            }
            if (rawSx <= 0.0 && maxX > minX) rawSx = maxX - minX;
            if (rawSy <= 0.0 && maxY > minY) rawSy = maxY - minY;
        }
        double holeDia = extractPositiveNumber(p, "hole_diameter");
        if (holeDia <= 0.0) holeDia = extractPositiveNumber(p, "drill");
        if (holeDia <= 0.0) {
            double dMm = extractPositiveNumber(p, "drill_mm");
            if (dMm > 0.0) holeDia = dMm * mmToCoord;
        }
        if (holeDia <= 0.0 && p.contains("hole") && p["hole"].is_array() &&
            p["hole"].size() >= 2 && p["hole"][1].is_number()) {
            holeDia = p["hole"][1].get<double>();
            if (p["hole"].size() >= 3 && p["hole"][2].is_number()) {
                holeDia = std::max(holeDia, p["hole"][2].get<double>());
            }
        }
        if (holeDia > 0.0) {
            rawSx = std::max(rawSx, holeDia);
            rawSy = std::max(rawSy, holeDia);
        }
        ap.sx = std::max(minPadSize, (rawSx > 0.0) ? rawSx : defaultPadSize);
        ap.sy = std::max(minPadSize, (rawSy > 0.0) ? rawSy : defaultPadSize);
        double rotDeg = 0.0;
        if (p.contains("rotation_deg") && p["rotation_deg"].is_number()) {
            rotDeg = p["rotation_deg"].get<double>();
        } else if (p.contains("rotation") && p["rotation"].is_number()) {
            rotDeg = p["rotation"].get<double>();
        }
        double rad = rotDeg * (3.14159265358979323846 / 180.0);
        ap.cosR = std::cos(rad);
        ap.sinR = std::sin(rad);
        ap.layer = (holeDia > 0.0) ? std::string("MultiLayer") : canonicalLayer(p, "TopLayer");
        if (!ap.ref.empty()) pcbRefs.insert(ap.ref);
        allPads.push_back(ap);
    };

    if (args.contains("pads") && args["pads"].is_array()) {
        for (const auto& p : args["pads"]) {
            ingestPadObj(p, "");
        }
    }
    bool hadTopLevelPads = !allPads.empty();
    if (args.contains("footprints") && args["footprints"].is_array()) {
        for (const auto& fp : args["footprints"]) {
            if (!fp.is_object()) continue;
            std::string fpRef = fp.value("reference", fp.value("ref", ""));
            if (!fpRef.empty()) pcbRefs.insert(fpRef);
            if (!hadTopLevelPads && fp.contains("pads") && fp["pads"].is_array()) {
                double fpx = fp.value("x", fp.value("x_mm", 0.0));
                double fpy = fp.value("y", fp.value("y_mm", 0.0));
                for (const auto& p : fp["pads"]) {
                    if (!p.is_object()) continue;
                    nlohmann::json copyP = p;
                    if (!copyP.contains("ref") && !copyP.contains("reference")) {
                        copyP["ref"] = fpRef;
                    }
                    if (!copyP.contains("x") && !copyP.contains("x_mm")) {
                        copyP["x"] = copyP.value("rel_x", copyP.value("dx", 0.0)) + fpx;
                        copyP["y"] = copyP.value("rel_y", copyP.value("dy", 0.0)) + fpy;
                    }
                    ingestPadObj(copyP, fpRef);
                }
            }
        }
    }

    std::vector<AuditTrack> allTracks;
    if (args.contains("tracks") && args["tracks"].is_array()) {
        for (const auto& t : args["tracks"]) {
            if (!t.is_object()) continue;
            std::string tid = t.value("id", "trk_" + std::to_string(allTracks.size() + 1));
            std::string tnet = t.value("net", "");
            std::string tlayer = canonicalLayer(t, "TopLayer");
            double tw = std::max(minTrackWidth, t.value("width", t.value("width_mm", defaultTrackWidth)));
            if (t.contains("points") && t["points"].is_array() && t["points"].size() >= 2 &&
                !t.contains("x1") && !t.contains("x1_mm")) {
                const auto& pts = t["points"];
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    if (pts[i].is_array() && pts[i].size() >= 2 &&
                        pts[i + 1].is_array() && pts[i + 1].size() >= 2) {
                        AuditTrack at;
                        at.id = tid;
                        at.net = tnet;
                        at.layer = tlayer;
                        at.x1 = pts[i][0].get<double>();
                        at.y1 = pts[i][1].get<double>();
                        at.x2 = pts[i + 1][0].get<double>();
                        at.y2 = pts[i + 1][1].get<double>();
                        at.width = tw;
                        allTracks.push_back(at);
                    }
                }
            } else {
                AuditTrack at;
                at.id = tid;
                at.net = tnet;
                at.layer = tlayer;
                at.x1 = t.value("x1", t.value("x1_mm", 0.0));
                at.y1 = t.value("y1", t.value("y1_mm", 0.0));
                at.x2 = t.value("x2", t.value("x2_mm", 0.0));
                at.y2 = t.value("y2", t.value("y2_mm", 0.0));
                at.width = tw;
                allTracks.push_back(at);
            }
        }
    }

    bool gpuKernelExecuted = false;
    if (allTracks.size() >= 64 && _capabilities.gpuAvailable && OpenClEngine::instance().isGpuAvailable()) {
        std::vector<float> segStartEnd;
        std::vector<float> segProps;
        for (const auto& t : allTracks) {
            segStartEnd.push_back(static_cast<float>(t.x1));
            segStartEnd.push_back(static_cast<float>(t.y1));
            segStartEnd.push_back(static_cast<float>(t.x2));
            segStartEnd.push_back(static_cast<float>(t.y2));
            segProps.push_back(static_cast<float>(t.width));
            segProps.push_back(t.layer == "TopLayer" ? 0.0f : (t.layer == "BottomLayer" ? 1.0f : -1.0f));
            segProps.push_back(0.0f);
            segProps.push_back(0.0f);
        }
        int outViolationCount = 0;
        std::vector<int> outViolations;
        if (OpenClEngine::instance().runSegmentClearance(
                static_cast<int>(allTracks.size()), 0.2f, segStartEnd, segProps,
                outViolationCount, outViolations)) {
            gpuKernelExecuted = true;
        }
    }

    std::vector<AuditVia> allVias;
    if (args.contains("vias") && args["vias"].is_array()) {
        for (const auto& v : args["vias"]) {
            if (!v.is_object()) continue;
            AuditVia av;
            av.id = v.value("id", "via_" + std::to_string(allVias.size() + 1));
            av.net = v.value("net", "");
            av.x = v.value("x", v.value("x_mm", 0.0));
            av.y = v.value("y", v.value("y_mm", 0.0));
            av.diameter = std::max(minViaDia, v.value("diameter", v.value("size_mm", defaultViaDia)));
            av.drill = std::max(minViaDrill, v.value("drill", v.value("drill_mm", defaultViaDrill)));
            allVias.push_back(av);
        }
    }

    std::vector<AuditPour> allPours;
    for (const char* pourKey : {"pours", "zones", "copper_pours"}) {
        if (!args.contains(pourKey) || !args[pourKey].is_array()) continue;
        for (const auto& z : args[pourKey]) {
            if (!z.is_object()) continue;
            AuditPour ap;
            ap.id = z.value("id", "pour_" + std::to_string(allPours.size() + 1));
            ap.net = z.value("net", "");
            ap.layer = canonicalLayer(z, "TopLayer");

            const nlohmann::json* polyArr = nullptr;
            for (const char* polyKey : {"polygon", "points", "complexPolygon", "complex_polygon", "outline", "source", "Source"}) {
                if (z.contains(polyKey)) {
                    if (z[polyKey].is_array()) {
                        polyArr = &z[polyKey];
                        break;
                    } else if (z[polyKey].is_object()) {
                        const auto& inner = z[polyKey];
                        for (const char* innerKey : {"polygon", "points", "outline", "source", "Source"}) {
                            if (inner.contains(innerKey) && inner[innerKey].is_array()) {
                                polyArr = &inner[innerKey];
                                break;
                            }
                        }
                        if (polyArr != nullptr) break;
                    }
                }
            }

            if (polyArr != nullptr) {
                for (size_t i = 0; i < polyArr->size(); ++i) {
                    const auto& tok = (*polyArr)[i];
                    if (tok.is_string()) {
                        std::string s = tok.get<std::string>();
                        for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        if (s == "ARC" && i + 1 < polyArr->size() && (*polyArr)[i + 1].is_number()) {
                            ++i;
                        }
                        continue;
                    }
                    if (tok.is_object()) {
                        if (tok.contains("x") && tok.contains("y") &&
                            tok["x"].is_number() && tok["y"].is_number()) {
                            ap.poly.emplace_back(tok["x"].get<double>(), tok["y"].get<double>());
                        } else if (tok.contains("x_mm") && tok.contains("y_mm") &&
                                   tok["x_mm"].is_number() && tok["y_mm"].is_number()) {
                            double px = isMilUnits ? (tok["x_mm"].get<double>() * mmToCoord) : tok["x_mm"].get<double>();
                            double py = isMilUnits ? (tok["y_mm"].get<double>() * mmToCoord) : tok["y_mm"].get<double>();
                            ap.poly.emplace_back(px, py);
                        }
                    } else if (tok.is_array() && tok.size() >= 2 &&
                               tok[0].is_number() && tok[1].is_number()) {
                        ap.poly.emplace_back(tok[0].get<double>(), tok[1].get<double>());
                    } else if (tok.is_number() && i + 1 < polyArr->size() &&
                               (*polyArr)[i + 1].is_number()) {
                        ap.poly.emplace_back(tok.get<double>(), (*polyArr)[i + 1].get<double>());
                        ++i;
                    }
                }
            }

            if (z.contains("x_min") && z.contains("y_min") && z.contains("x_max") && z.contains("y_max") &&
                z["x_min"].is_number() && z["y_min"].is_number() &&
                z["x_max"].is_number() && z["y_max"].is_number()) {
                ap.hasBbox = true;
                ap.xMin = z["x_min"].get<double>();
                ap.yMin = z["y_min"].get<double>();
                ap.xMax = z["x_max"].get<double>();
                ap.yMax = z["y_max"].get<double>();
            } else if (z.contains("min_x") && z.contains("min_y") && z.contains("max_x") && z.contains("max_y") &&
                       z["min_x"].is_number() && z["min_y"].is_number() &&
                       z["max_x"].is_number() && z["max_y"].is_number()) {
                ap.hasBbox = true;
                ap.xMin = z["min_x"].get<double>();
                ap.yMin = z["min_y"].get<double>();
                ap.xMax = z["max_x"].get<double>();
                ap.yMax = z["max_y"].get<double>();
            } else if (z.contains("bbox") && z["bbox"].is_object()) {
                const auto& bb = z["bbox"];
                if (bb.contains("x_min") && bb.contains("y_min") && bb.contains("x_max") && bb.contains("y_max")) {
                    ap.hasBbox = true;
                    ap.xMin = bb.value("x_min", 0.0);
                    ap.yMin = bb.value("y_min", 0.0);
                    ap.xMax = bb.value("x_max", 0.0);
                    ap.yMax = bb.value("y_max", 0.0);
                } else if (bb.contains("min_x") && bb.contains("min_y") && bb.contains("max_x") && bb.contains("max_y")) {
                    ap.hasBbox = true;
                    ap.xMin = bb.value("min_x", 0.0);
                    ap.yMin = bb.value("min_y", 0.0);
                    ap.xMax = bb.value("max_x", 0.0);
                    ap.yMax = bb.value("max_y", 0.0);
                }
            } else if (z.contains("x") && z.contains("y") &&
                       (z.contains("width") || z.contains("w")) &&
                       (z.contains("height") || z.contains("h"))) {
                double zx = z["x"].is_number() ? z["x"].get<double>() : 0.0;
                double zy = z["y"].is_number() ? z["y"].get<double>() : 0.0;
                double zw = z.contains("width") ? (z["width"].is_number() ? z["width"].get<double>() : 0.0)
                                                : (z["w"].is_number() ? z["w"].get<double>() : 0.0);
                double zh = z.contains("height") ? (z["height"].is_number() ? z["height"].get<double>() : 0.0)
                                                 : (z["h"].is_number() ? z["h"].get<double>() : 0.0);
                if (zw > 0.0 && zh > 0.0) {
                    ap.hasBbox = true;
                    ap.xMin = zx;
                    ap.yMin = zy;
                    ap.xMax = zx + zw;
                    ap.yMax = zy + zh;
                }
            }

            if (ap.hasBbox) {
                if (ap.xMin > ap.xMax) std::swap(ap.xMin, ap.xMax);
                if (ap.yMin > ap.yMax) std::swap(ap.yMin, ap.yMax);
                if (ap.poly.empty()) {
                    ap.poly = {
                        {ap.xMin, ap.yMin},
                        {ap.xMax, ap.yMin},
                        {ap.xMax, ap.yMax},
                        {ap.xMin, ap.yMax}
                    };
                }
            } else if (!ap.poly.empty()) {
                ap.hasBbox = true;
                ap.xMin = ap.xMax = ap.poly[0].first;
                ap.yMin = ap.yMax = ap.poly[0].second;
                for (const auto& pt : ap.poly) {
                    ap.xMin = std::min(ap.xMin, pt.first);
                    ap.xMax = std::max(ap.xMax, pt.first);
                    ap.yMin = std::min(ap.yMin, pt.second);
                    ap.yMax = std::max(ap.yMax, pt.second);
                }
            } else if (!ap.net.empty()) {
                // Fallback when an extension omits complexPolygon points: derive board extent
                // from board_outline or allPads so full-board pours with a valid net are preserved.
                double fbMinX = std::numeric_limits<double>::infinity();
                double fbMinY = std::numeric_limits<double>::infinity();
                double fbMaxX = -std::numeric_limits<double>::infinity();
                double fbMaxY = -std::numeric_limits<double>::infinity();
                if (args.contains("board_outline") && args["board_outline"].is_array()) {
                    for (const auto& seg : args["board_outline"]) {
                        if (!seg.is_object()) continue;
                        if (seg.contains("x1") && seg["x1"].is_number() &&
                            seg.contains("y1") && seg["y1"].is_number() &&
                            seg.contains("x2") && seg["x2"].is_number() &&
                            seg.contains("y2") && seg["y2"].is_number()) {
                            fbMinX = std::min({fbMinX, seg["x1"].get<double>(), seg["x2"].get<double>()});
                            fbMinY = std::min({fbMinY, seg["y1"].get<double>(), seg["y2"].get<double>()});
                            fbMaxX = std::max({fbMaxX, seg["x1"].get<double>(), seg["x2"].get<double>()});
                            fbMaxY = std::max({fbMaxY, seg["y1"].get<double>(), seg["y2"].get<double>()});
                        }
                    }
                }
                if (!(fbMaxX >= fbMinX && fbMaxY >= fbMinY) && !allPads.empty()) {
                    for (const auto& pd : allPads) {
                        fbMinX = std::min(fbMinX, pd.x - pd.sx);
                        fbMinY = std::min(fbMinY, pd.y - pd.sy);
                        fbMaxX = std::max(fbMaxX, pd.x + pd.sx);
                        fbMaxY = std::max(fbMaxY, pd.y + pd.sy);
                    }
                }
                if (fbMaxX >= fbMinX && fbMaxY >= fbMinY) {
                    ap.hasBbox = true;
                    ap.xMin = fbMinX;
                    ap.yMin = fbMinY;
                    ap.xMax = fbMaxX;
                    ap.yMax = fbMaxY;
                    ap.poly = {
                        {ap.xMin, ap.yMin},
                        {ap.xMax, ap.yMin},
                        {ap.xMax, ap.yMax},
                        {ap.xMin, ap.yMax}
                    };
                }
            }

            if (!ap.net.empty() && (ap.poly.size() >= 3 || ap.hasBbox)) {
                allPours.push_back(ap);
            }
        }
    }

    auto pointInOrNearPour = [&](const AuditPour& pour, double px, double py, double radius) -> bool {
        if (pour.hasBbox) {
            if (px < pour.xMin - radius || px > pour.xMax + radius ||
                py < pour.yMin - radius || py > pour.yMax + radius) {
                return false;
            }
        }
        if (pour.poly.size() >= 3) {
            bool inside = false;
            size_t n = pour.poly.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                double xi = pour.poly[i].first;
                double yi = pour.poly[i].second;
                double xj = pour.poly[j].first;
                double yj = pour.poly[j].second;
                bool intersect = ((yi > py) != (yj > py)) &&
                                 (px < (xj - xi) * (py - yi) / ((yj - yi) + ((yj >= yi) ? 1e-15 : -1e-15)) + xi);
                if (intersect) inside = !inside;
                if (distPointToSeg(px, py, xj, yj, xi, yi) <= radius) {
                    return true;
                }
            }
            return inside;
        }
        return pour.hasBbox;
    };

    // 1. Unassigned pads (ignoring mechanical mounting holes H*, MH*, MP)
    nlohmann::json unassignedPadsJson = nlohmann::json::array();
    std::map<std::string, std::vector<size_t>> netPadsMap;
    for (size_t i = 0; i < allPads.size(); ++i) {
        const auto& p = allPads[i];
        if (p.net.empty()) {
            bool isMountingHole = (p.ref.rfind("H", 0) == 0 || p.ref.rfind("MH", 0) == 0 ||
                                   p.pad == "MP" || p.pad == "MH" || p.id.rfind("mh", 0) == 0);
            if (!isMountingHole) {
                unassignedPadsJson.push_back({
                    {"id", p.id},
                    {"ref", p.ref},
                    {"pad", p.pad},
                    {"x", p.x},
                    {"y", p.y},
                    {"layer", p.layer}
                });
            }
        } else {
            netPadsMap[p.net].push_back(i);
        }
    }

    // 2. Per-net 3D Layer-Aware Union-Find connectivity analysis (including copper pours/zones)
    int totalNets = 0;
    int fullyRoutedNets = 0;
    nlohmann::json unroutedNetsJson = nlohmann::json::array();

    for (const auto& kv : netPadsMap) {
        const std::string& netName = kv.first;
        const auto& padIndices = kv.second;
        if (padIndices.size() < 2) continue;
        ++totalNets;

        std::vector<size_t> trkIndices;
        for (size_t i = 0; i < allTracks.size(); ++i) {
            if (allTracks[i].net == netName || allTracks[i].net.empty()) {
                trkIndices.push_back(i);
            }
        }
        std::vector<size_t> viaIndices;
        for (size_t i = 0; i < allVias.size(); ++i) {
            if (allVias[i].net == netName || allVias[i].net.empty()) {
                viaIndices.push_back(i);
            }
        }
        std::vector<size_t> pourIndices;
        for (size_t i = 0; i < allPours.size(); ++i) {
            if (allPours[i].net == netName) {
                pourIndices.push_back(i);
            }
        }

        size_t nP = padIndices.size();
        size_t nT = trkIndices.size();
        size_t nV = viaIndices.size();
        size_t nZ = pourIndices.size();
        size_t totalNodes = nP + nT + nV + nZ;
        std::vector<size_t> ufParent(totalNodes);
        for (size_t i = 0; i < totalNodes; ++i) ufParent[i] = i;

        auto ufFind = [&](size_t x) {
            size_t r = x;
            while (ufParent[r] != r) r = ufParent[r];
            while (ufParent[x] != r) {
                size_t nxt = ufParent[x];
                ufParent[x] = r;
                x = nxt;
            }
            return r;
        };
        auto ufUnion = [&](size_t a, size_t b) {
            size_t ra = ufFind(a);
            size_t rb = ufFind(b);
            if (ra != rb) ufParent[rb] = ra;
        };

        // Pad-to-Pad
        for (size_t i = 0; i < nP; ++i) {
            const auto& pa = allPads[padIndices[i]];
            for (size_t j = i + 1; j < nP; ++j) {
                const auto& pb = allPads[padIndices[j]];
                if (!layersMatch(pa.layer, pb.layer)) continue;
                if (padsTouch(pa, pb, touchTol)) {
                    ufUnion(i, j);
                }
            }
        }

        // Pad-to-Track
        for (size_t i = 0; i < nP; ++i) {
            const auto& pa = allPads[padIndices[i]];
            for (size_t j = 0; j < nT; ++j) {
                const auto& trk = allTracks[trkIndices[j]];
                if (!layersMatch(pa.layer, trk.layer)) continue;
                double d = distSegToPad(trk.x1, trk.y1, trk.x2, trk.y2, pa);
                if (d <= 0.5 * trk.width + touchTol) {
                    ufUnion(i, nP + j);
                }
            }
        }

        // Pad-to-Via (vias span all layers)
        for (size_t i = 0; i < nP; ++i) {
            const auto& pa = allPads[padIndices[i]];
            for (size_t k = 0; k < nV; ++k) {
                const auto& via = allVias[viaIndices[k]];
                if (distPointToPad(via.x, via.y, pa) <= 0.5 * via.diameter + touchTol) {
                    ufUnion(i, nP + nT + k);
                }
            }
        }

        // Track-to-Via (vias span all layers)
        for (size_t j = 0; j < nT; ++j) {
            const auto& trk = allTracks[trkIndices[j]];
            for (size_t k = 0; k < nV; ++k) {
                const auto& via = allVias[viaIndices[k]];
                double d = distPointToSeg(via.x, via.y, trk.x1, trk.y1, trk.x2, trk.y2);
                if (d <= 0.5 * via.diameter + 0.5 * trk.width + touchTol) {
                    ufUnion(nP + j, nP + nT + k);
                }
            }
        }

        // Track-to-Track on matching layer
        for (size_t i = 0; i < nT; ++i) {
            const auto& ta = allTracks[trkIndices[i]];
            for (size_t j = i + 1; j < nT; ++j) {
                const auto& tb = allTracks[trkIndices[j]];
                if (!layersMatch(ta.layer, tb.layer)) continue;
                double d = distSegToSeg(ta.x1, ta.y1, ta.x2, ta.y2, tb.x1, tb.y1, tb.x2, tb.y2);
                if (d <= 0.5 * (ta.width + tb.width) + touchTol) {
                    ufUnion(nP + i, nP + j);
                }
            }
        }

        // Via-to-Via
        for (size_t i = 0; i < nV; ++i) {
            const auto& va = allVias[viaIndices[i]];
            for (size_t j = i + 1; j < nV; ++j) {
                const auto& vb = allVias[viaIndices[j]];
                if (std::hypot(va.x - vb.x, va.y - vb.y) <= 0.5 * (va.diameter + vb.diameter) + touchTol) {
                    ufUnion(nP + nT + i, nP + nT + j);
                }
            }
        }

        // Copper Pour / Zone unions (Pad-to-Pour, Track-to-Pour, Via-to-Pour)
        for (size_t z = 0; z < nZ; ++z) {
            const auto& pour = allPours[pourIndices[z]];
            size_t pourNode = nP + nT + nV + z;
            for (size_t i = 0; i < nP; ++i) {
                const auto& pa = allPads[padIndices[i]];
                if (!layersMatch(pa.layer, pour.layer)) continue;
                double ra = 0.5 * std::max(pa.sx, pa.sy);
                if (pointInOrNearPour(pour, pa.x, pa.y, ra + touchTol)) {
                    ufUnion(i, pourNode);
                }
            }
            for (size_t j = 0; j < nT; ++j) {
                const auto& trk = allTracks[trkIndices[j]];
                if (!layersMatch(trk.layer, pour.layer)) continue;
                double rt = 0.5 * trk.width + touchTol;
                if (pointInOrNearPour(pour, trk.x1, trk.y1, rt) ||
                    pointInOrNearPour(pour, trk.x2, trk.y2, rt)) {
                    ufUnion(nP + j, pourNode);
                }
            }
            for (size_t k = 0; k < nV; ++k) {
                const auto& via = allVias[viaIndices[k]];
                if (pointInOrNearPour(pour, via.x, via.y, 0.5 * via.diameter + touchTol)) {
                    ufUnion(nP + nT + k, pourNode);
                }
            }
        }

        std::map<size_t, std::vector<size_t>> islands;
        for (size_t i = 0; i < nP; ++i) {
            islands[ufFind(i)].push_back(padIndices[i]);
        }

        if (islands.size() == 1) {
            ++fullyRoutedNets;
        } else {
            double minDist = std::numeric_limits<double>::infinity();
            size_t bestA = padIndices[0];
            size_t bestB = padIndices[1];
            for (auto itA = islands.begin(); itA != islands.end(); ++itA) {
                auto itB = itA;
                ++itB;
                for (; itB != islands.end(); ++itB) {
                    for (size_t idxA : itA->second) {
                        for (size_t idxB : itB->second) {
                            double d = std::hypot(allPads[idxA].x - allPads[idxB].x,
                                                  allPads[idxA].y - allPads[idxB].y);
                            if (d < minDist) {
                                minDist = d;
                                bestA = idxA;
                                bestB = idxB;
                            }
                        }
                    }
                }
            }
            double distMm = isMilUnits ? (minDist * 0.0254) : minDist;
            unroutedNetsJson.push_back({
                {"net", netName},
                {"island_count", static_cast<int>(islands.size())},
                {"pad_count", static_cast<int>(nP)},
                {"airwire", {
                    {"from_ref", allPads[bestA].ref},
                    {"from_pad", allPads[bestA].pad},
                    {"from_x", allPads[bestA].x},
                    {"from_y", allPads[bestA].y},
                    {"to_ref", allPads[bestB].ref},
                    {"to_pad", allPads[bestB].pad},
                    {"to_x", allPads[bestB].x},
                    {"to_y", allPads[bestB].y},
                    {"distance_mm", std::round(distMm * 100.0) / 100.0}
                }}
            });
        }
    }

    // 3. Dangling track endpoints detection
    nlohmann::json danglingTracksJson = nlohmann::json::array();
    for (size_t i = 0; i < allTracks.size(); ++i) {
        const auto& trk = allTracks[i];
        double endPts[2][2] = {{trk.x1, trk.y1}, {trk.x2, trk.y2}};
        for (int ep = 0; ep < 2; ++ep) {
            double ex = endPts[ep][0];
            double ey = endPts[ep][1];
            bool anchored = false;

            for (const auto& pad : allPads) {
                if (!trk.net.empty() && !pad.net.empty() && trk.net != pad.net) continue;
                if (!layersMatch(trk.layer, pad.layer)) continue;
                if (distPointToPad(ex, ey, pad) <= 0.5 * trk.width + touchTol) {
                    anchored = true;
                    break;
                }
            }
            if (!anchored) {
                for (const auto& via : allVias) {
                    if (!trk.net.empty() && !via.net.empty() && trk.net != via.net) continue;
                    if (std::hypot(ex - via.x, ey - via.y) <= 0.5 * via.diameter + 0.5 * trk.width + touchTol) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (!anchored) {
                for (size_t j = 0; j < allTracks.size(); ++j) {
                    if (j == i) continue;
                    const auto& other = allTracks[j];
                    if (!trk.net.empty() && !other.net.empty() && trk.net != other.net) continue;
                    if (!layersMatch(trk.layer, other.layer)) continue;
                    double d = distPointToSeg(ex, ey, other.x1, other.y1, other.x2, other.y2);
                    if (d <= 0.5 * (trk.width + other.width) + touchTol) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (!anchored) {
                for (const auto& pour : allPours) {
                    if (!trk.net.empty() && !pour.net.empty() && trk.net != pour.net) continue;
                    if (!layersMatch(trk.layer, pour.layer)) continue;
                    if (pointInOrNearPour(pour, ex, ey, 0.5 * trk.width + touchTol)) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (!anchored) {
                danglingTracksJson.push_back({
                    {"id", trk.id},
                    {"net", trk.net},
                    {"layer", trk.layer},
                    {"x", ex},
                    {"y", ey}
                });
            }
        }
    }

    // 4. Schematic-to-PCB component parity check (with multi-sheet deduplication)
    for (const char* compKey : {"pcb_components", "components"}) {
        if (args.contains(compKey) && args[compKey].is_array()) {
            for (const auto& pc : args[compKey]) {
                if (!pc.is_object()) continue;
                std::string ref = pc.value("reference", pc.value("ref", pc.value("designator", "")));
                if (!ref.empty()) pcbRefs.insert(ref);
            }
        }
    }

    nlohmann::json unsyncedJson = nlohmann::json::array();
    std::set<std::string> seenSchRefs;
    if (args.contains("schematic_components") && args["schematic_components"].is_array()) {
        for (const auto& sc : args["schematic_components"]) {
            if (!sc.is_object()) continue;
            std::string ref = sc.value("reference", sc.value("ref", sc.value("designator", "")));
            std::string compType = sc.value("component_type", sc.value("type", ""));
            std::string fp = sc.value("footprint", "");
            if (ref.empty() || compType == "netport" || compType == "frame" || compType == "power" ||
                compType == "netflag" || ref.rfind("FRAME", 0) == 0 || ref.rfind("#", 0) == 0 ||
                ref == "GND" || ref == "VCC") {
                continue;
            }
            if (fp.empty() && !compType.empty() && compType != "part") {
                continue;
            }
            if (!seenSchRefs.insert(ref).second) {
                continue;
            }
            if (pcbRefs.find(ref) == pcbRefs.end()) {
                unsyncedJson.push_back({
                    {"reference", ref},
                    {"value", sc.value("value", "")},
                    {"footprint", fp}
                });
            }
        }
    }

    // 5. Decoupling capacitor placement distance check (default max_decoupling_distance_mm = 5.0 mm)
    double maxDecouplingDistMm = args.value("max_decoupling_distance_mm", 5.0);
    if (maxDecouplingDistMm <= 0.0) {
        maxDecouplingDistMm = 5.0;
    }

    auto isPositiveSupplyNet = [](const std::string& rawNet) -> bool {
        if (rawNet.empty()) return false;
        std::string u = rawNet;
        for (char& ch : u) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        if (u == "GND" || u == "AGND" || u == "PGND" || u == "DGND" ||
            u == "VSS" || u == "VSSA" || u == "GNDA" ||
            u.rfind("GND", 0) == 0 || u.rfind("VSS", 0) == 0) {
            return false;
        }
        if (u[0] == '+' ||
            u.find("VCC") != std::string::npos ||
            u.find("VDD") != std::string::npos ||
            u.find("VBUS") != std::string::npos ||
            u.find("VBAT") != std::string::npos ||
            u.find("VSYS") != std::string::npos ||
            u.find("VIN") != std::string::npos ||
            u.find("3V3") != std::string::npos ||
            u.find("3.3V") != std::string::npos ||
            u.find("5V") != std::string::npos ||
            u.find("12V") != std::string::npos ||
            u.find("1V8") != std::string::npos ||
            u.find("1.8V") != std::string::npos ||
            u.find("2V5") != std::string::npos ||
            u.find("2.5V") != std::string::npos) {
            return true;
        }
        return false;
    };

    auto isIcRef = [](const std::string& ref) -> bool {
        if (ref.size() >= 2 && (ref[0] == 'U' || ref[0] == 'u') &&
            std::isdigit(static_cast<unsigned char>(ref[1]))) {
            return true;
        }
        if (ref.size() >= 3 && (ref[0] == 'I' || ref[0] == 'i') &&
            (ref[1] == 'C' || ref[1] == 'c') &&
            std::isdigit(static_cast<unsigned char>(ref[2]))) {
            return true;
        }
        return false;
    };

    auto isCapRef = [](const std::string& ref) -> bool {
        return ref.size() >= 2 && (ref[0] == 'C' || ref[0] == 'c') &&
               std::isdigit(static_cast<unsigned char>(ref[1]));
    };

    nlohmann::json decouplingWarningsJson = nlohmann::json::array();
    std::set<std::pair<std::string, std::string>> checkedIcSupplyPairs;
    for (const auto& icPad : allPads) {
        if (!isIcRef(icPad.ref) || !isPositiveSupplyNet(icPad.net)) {
            continue;
        }
        if (!checkedIcSupplyPairs.insert({icPad.ref, icPad.net}).second) {
            continue;
        }
        double bestDistMm = std::numeric_limits<double>::infinity();
        std::string bestCapRef;
        for (const auto& icP : allPads) {
            if (icP.ref != icPad.ref || icP.net != icPad.net) continue;
            for (const auto& capP : allPads) {
                if (!isCapRef(capP.ref) || capP.net != icPad.net) continue;
                double dMm = std::hypot(icP.x - capP.x, icP.y - capP.y) / mmToCoord;
                if (dMm < bestDistMm) {
                    bestDistMm = dMm;
                    bestCapRef = capP.ref;
                }
            }
        }
        if (!bestCapRef.empty() && bestDistMm > maxDecouplingDistMm) {
            decouplingWarningsJson.push_back({
                {"ic_reference", icPad.ref},
                {"cap_reference", bestCapRef},
                {"net", icPad.net},
                {"distance_mm", std::round(bestDistMm * 1000.0) / 1000.0},
                {"limit_mm", maxDecouplingDistMm}
            });
        }
    }

    int unroutedCount = static_cast<int>(unroutedNetsJson.size());
    int danglingCount = static_cast<int>(danglingTracksJson.size());
    int unassignedCount = static_cast<int>(unassignedPadsJson.size());
    int unsyncedCount = static_cast<int>(unsyncedJson.size());
    int decouplingWarnCount = static_cast<int>(decouplingWarningsJson.size());
    int poursCount = static_cast<int>(allPours.size());
    bool allConnected = (totalNets == 0) ? (danglingCount == 0) : (unroutedCount == 0 && danglingCount == 0);
    bool schPcbSynced = (unsyncedCount == 0);

    outSuccess = true;
    outSummary = "Nets " + std::to_string(fullyRoutedNets) + "/" + std::to_string(totalNets) +
                 " connected • " + std::to_string(unroutedCount) + " broken • " +
                 std::to_string(danglingCount) + " dangling • " +
                 std::to_string(unassignedCount) + " unassigned pads • " +
                 std::to_string(unsyncedCount) + " unsynced components";

    nlohmann::json res = {
        {"ok", true},
        {"all_connected", allConnected},
        {"schematic_pcb_synced", schPcbSynced},
        {"total_nets", totalNets},
        {"fully_routed_nets", fullyRoutedNets},
        {"unrouted_nets_count", unroutedCount},
        {"unrouted_nets", unroutedNetsJson},
        {"dangling_tracks_count", danglingCount},
        {"dangling_tracks", danglingTracksJson},
        {"pours_count", poursCount},
        {"unassigned_pads_count", unassignedCount},
        {"unassigned_pads", unassignedPadsJson},
        {"unsynced_schematic_components_count", unsyncedCount},
        {"unsynced_schematic_components", unsyncedJson},
        {"decoupling_distance_warnings_count", decouplingWarnCount},
        {"decoupling_distance_warnings", decouplingWarningsJson},
        {"summary", outSummary}
    };
    if (gpuKernelExecuted) {
        res["gpu_kernel_executed"] = true;
    }
    attachProvenance(res, "eda_calc_pcb_connectivity_audit");
    return res;
}

nlohmann::json EdaAccelerators::calcSchematicErcAudit(const nlohmann::json& args,
                                                      bool& outSuccess,
                                                      std::string& outSummary) const {
    if (!args.is_object()) {
        outSuccess = false;
        outSummary = "eda_calc_schematic_erc_audit requires a JSON object payload";
        return {{"ok", false}, {"error", outSummary}};
    }

    auto distPointToSeg = [](double px, double py,
                             double x1, double y1,
                             double x2, double y2) -> double {
        double dx = x2 - x1;
        double dy = y2 - y1;
        double len2 = dx * dx + dy * dy;
        if (len2 < 1e-12) {
            return std::hypot(px - x1, py - y1);
        }
        double t = std::clamp(((px - x1) * dx + (py - y1) * dy) / len2, 0.0, 1.0);
        return std::hypot(px - (x1 + t * dx), py - (y1 + t * dy));
    };

    struct SchWire {
        std::string id;
        std::string net;
        std::vector<std::pair<double, double>> pts;
    };

    struct SchPin {
        std::string id;
        std::string compId;
        std::string ref;
        std::string number;
        std::string name;
        std::string net;
        double x = 0.0;
        double rawY = 0.0;
        double normY = 0.0;
        bool noConnected = false;
        bool isPartPin = true;
        bool isNetFlag = false;
        std::string netFlagName;
        bool touchesWire = false;
    };

    const double kTol = std::max(1e-3, args.value("tolerance", 1.5));
    std::vector<SchWire> wires;
    bool anyWireVertex = false;
    bool allWireYNonNegative = true;

    if (args.contains("wires") && args["wires"].is_array()) {
        for (const auto& w : args["wires"]) {
            if (!w.is_object()) continue;
            std::string wId = w.value("id", "w_" + std::to_string(wires.size() + 1));
            std::string wNet = w.value("net", "");
            std::vector<std::pair<double, double>> rawPts;
            for (const char* lineKey : {"line", "points"}) {
                if (w.contains(lineKey) && w[lineKey].is_array()) {
                    const auto& arr = w[lineKey];
                    if (!arr.empty() && arr[0].is_number()) {
                        for (size_t i = 0; i + 1 < arr.size(); i += 2) {
                            if (arr[i].is_number() && arr[i + 1].is_number()) {
                                double wx = arr[i].get<double>();
                                double wy = arr[i + 1].get<double>();
                                rawPts.push_back({wx, wy});
                                anyWireVertex = true;
                                if (wy < 0.0) allWireYNonNegative = false;
                            }
                        }
                    } else {
                        for (const auto& pt : arr) {
                            if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() && pt[1].is_number()) {
                                double wx = pt[0].get<double>();
                                double wy = pt[1].get<double>();
                                rawPts.push_back({wx, wy});
                                anyWireVertex = true;
                                if (wy < 0.0) allWireYNonNegative = false;
                            } else if (pt.is_object() && pt.contains("x") && pt.contains("y") &&
                                       pt["x"].is_number() && pt["y"].is_number()) {
                                double wx = pt["x"].get<double>();
                                double wy = pt["y"].get<double>();
                                rawPts.push_back({wx, wy});
                                anyWireVertex = true;
                                if (wy < 0.0) allWireYNonNegative = false;
                            }
                        }
                    }
                    if (!rawPts.empty()) break;
                }
            }
            if (rawPts.empty() && w.contains("x1") && w.contains("y1") &&
                w.contains("x2") && w.contains("y2")) {
                double x1 = w.value("x1", 0.0);
                double y1 = w.value("y1", 0.0);
                double x2 = w.value("x2", 0.0);
                double y2 = w.value("y2", 0.0);
                rawPts.push_back({x1, y1});
                rawPts.push_back({x2, y2});
                anyWireVertex = true;
                if (y1 < 0.0 || y2 < 0.0) allWireYNonNegative = false;
            }
            if (rawPts.size() >= 4 && rawPts.size() % 2 == 0) {
                bool allPairsOrtho = true;
                for (size_t k = 0; k + 1 < rawPts.size(); k += 2) {
                    if (std::fabs(rawPts[k].first - rawPts[k + 1].first) > kTol &&
                        std::fabs(rawPts[k].second - rawPts[k + 1].second) > kTol) {
                        allPairsOrtho = false;
                        break;
                    }
                }
                bool anyOddToEvenNonOrtho = false;
                for (size_t k = 1; k + 1 < rawPts.size(); k += 2) {
                    if (std::fabs(rawPts[k].first - rawPts[k + 1].first) > kTol &&
                        std::fabs(rawPts[k].second - rawPts[k + 1].second) > kTol) {
                        anyOddToEvenNonOrtho = true;
                        break;
                    }
                }
                bool hasRepeatedVertex = false;
                for (size_t a = 0; a < rawPts.size() && !hasRepeatedVertex; ++a) {
                    for (size_t b = a + 1; b < rawPts.size(); ++b) {
                        if (std::hypot(rawPts[a].first - rawPts[b].first,
                                       rawPts[a].second - rawPts[b].second) <= kTol) {
                            hasRepeatedVertex = true;
                            break;
                        }
                    }
                }
                if (allPairsOrtho && (anyOddToEvenNonOrtho || hasRepeatedVertex)) {
                    for (size_t k = 0; k + 1 < rawPts.size(); k += 2) {
                        SchWire sw;
                        sw.id = wId;
                        sw.net = wNet;
                        sw.pts = {rawPts[k], rawPts[k + 1]};
                        wires.push_back(std::move(sw));
                    }
                    continue;
                }
            }
            if (rawPts.size() > 2) {
                std::vector<std::pair<double, double>> curSeg = {rawPts[0]};
                for (size_t k = 1; k < rawPts.size(); ++k) {
                    double dx = std::fabs(rawPts[k].first - curSeg.back().first);
                    double dy = std::fabs(rawPts[k].second - curSeg.back().second);
                    if (dx > kTol && dy > kTol) {
                        if (curSeg.size() >= 2) {
                            SchWire sw;
                            sw.id = wId;
                            sw.net = wNet;
                            sw.pts = std::move(curSeg);
                            wires.push_back(std::move(sw));
                        }
                        curSeg = {rawPts[k]};
                    } else {
                        curSeg.push_back(rawPts[k]);
                    }
                }
                if (curSeg.size() >= 2) {
                    SchWire sw;
                    sw.id = wId;
                    sw.net = wNet;
                    sw.pts = std::move(curSeg);
                    wires.push_back(std::move(sw));
                }
            } else if (rawPts.size() >= 2) {
                SchWire sw;
                sw.id = wId;
                sw.net = wNet;
                sw.pts = std::move(rawPts);
                wires.push_back(std::move(sw));
            }
        }
    }

    bool isMultiSheetSchematic = false;
    if (args.contains("project") && args["project"].is_object() &&
        args["project"].contains("data") && args["project"]["data"].is_array()) {
        for (const auto& dItem : args["project"]["data"]) {
            if (dItem.is_object() && dItem.contains("schematic") &&
                dItem["schematic"].is_object() && dItem["schematic"].contains("page") &&
                dItem["schematic"]["page"].is_array() && dItem["schematic"]["page"].size() > 1) {
                isMultiSheetSchematic = true;
                break;
            }
        }
    }

    int totalPartComponents = 0;
    int totalPartPins = 0;
    nlohmann::json componentIssuesJson = nlohmann::json::array();
    std::map<std::string, int> partRefCounts;
    std::vector<SchPin> pins;

    struct SchPartComp {
        std::string id;
        std::string ref;
        std::string val;
        double x = 0.0;
        double y = 0.0;
        double normY = 0.0;
        double minX = 0.0;
        double minY = 0.0;
        double maxX = 0.0;
        double maxY = 0.0;
        std::vector<size_t> pinIndices;
    };
    std::vector<SchPartComp> partComps;

    if (args.contains("components") && args["components"].is_array()) {
        for (const auto& c : args["components"]) {
            if (!c.is_object()) continue;
            bool onActiveSheet = c.value("on_active_sheet", true);
            if (!onActiveSheet) {
                isMultiSheetSchematic = true;
            }
            std::string cid = c.value("id", "");
            std::string ref = c.value("reference", c.value("ref", c.value("designator", "")));
            std::string val = c.value("value", c.value("name", ""));
            std::string compType = c.value("component_type", c.value("type", ""));
            std::string fp = c.value("footprint", "");
            std::string uid = c.value("unique_id", c.value("uniqueId", ""));
            double cx = c.value("x", 0.0);
            double cy = c.value("y", 0.0);
            double normCy = (cy < 0.0 && anyWireVertex && allWireYNonNegative) ? std::fabs(cy) : cy;

            bool isFrame = (compType == "frame" || compType == "sheet" || ref.rfind("FRAME", 0) == 0);
            if (isFrame) continue;

            bool isNetFlag = (compType == "netflag" || compType == "netport" ||
                              compType == "netlabel" || compType == "power" ||
                              ref.rfind("#", 0) == 0 || ref == "GND" || ref == "VCC");
            bool isPart = (!isNetFlag && (compType == "part" || compType.empty()));

            if (isPart) {
                ++totalPartComponents;
                if (ref.empty() || ref.find('?') != std::string::npos) {
                    componentIssuesJson.push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"issue", "unannotated_designator"},
                        {"x", cx},
                        {"y", cy}
                    });
                } else {
                    if (++partRefCounts[ref] > 1) {
                        componentIssuesJson.push_back({
                            {"id", cid},
                            {"reference", ref},
                            {"issue", "duplicate_designator"},
                            {"x", cx},
                            {"y", cy}
                        });
                    }
                }
                if (uid.empty()) {
                    componentIssuesJson.push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"issue", "missing_unique_id"},
                        {"x", cx},
                        {"y", cy}
                    });
                }
                if (fp.empty()) {
                    componentIssuesJson.push_back({
                        {"id", cid},
                        {"reference", ref},
                        {"issue", "missing_footprint"},
                        {"x", cx},
                        {"y", cy}
                    });
                }
            }

            if (!onActiveSheet) continue;

            std::vector<size_t> thisCompPins;
            if (c.contains("pins") && c["pins"].is_array() && !c["pins"].empty()) {
                for (const auto& p : c["pins"]) {
                    if (!p.is_object()) continue;
                    SchPin sp;
                    sp.id = p.value("id", cid + "_" + p.value("number", ""));
                    sp.compId = cid;
                    sp.ref = ref;
                    sp.number = p.value("number", p.value("pin_number", ""));
                    sp.name = p.value("name", p.value("pin_name", ""));
                    sp.net = p.value("net", "");
                    sp.x = p.value("x", cx);
                    sp.rawY = p.value("y", cy);
                    sp.normY = (sp.rawY < 0.0 && ((anyWireVertex && allWireYNonNegative) || cy > 0.0))
                                   ? std::fabs(sp.rawY)
                                   : sp.rawY;
                    sp.noConnected = p.value("no_connected", p.value("noConnected", false));
                    sp.isPartPin = isPart;
                    sp.isNetFlag = isNetFlag;
                    if (isNetFlag) {
                        if (!val.empty()) sp.netFlagName = val;
                        else if (!sp.net.empty()) sp.netFlagName = sp.net;
                        else if (!sp.name.empty()) sp.netFlagName = sp.name;
                    }
                    if (isPart) {
                        ++totalPartPins;
                    }
                    thisCompPins.push_back(pins.size());
                    pins.push_back(sp);
                }
            } else if (isNetFlag) {
                std::string nfNet = val;
                if (nfNet.empty()) nfNet = c.value("net", ref);
                SchPin sp;
                sp.id = cid.empty() ? ("nf_" + std::to_string(pins.size() + 1)) : (cid + "_p1");
                sp.compId = cid;
                sp.ref = ref;
                sp.number = "1";
                sp.name = nfNet;
                sp.net = nfNet;
                sp.x = cx;
                sp.rawY = cy;
                sp.normY = normCy;
                sp.noConnected = false;
                sp.isPartPin = false;
                sp.isNetFlag = true;
                sp.netFlagName = nfNet;
                pins.push_back(sp);
            }

            if (isPart) {
                SchPartComp pc;
                pc.id = cid;
                pc.ref = ref;
                pc.val = val;
                pc.x = cx;
                pc.y = cy;
                pc.normY = normCy;
                pc.pinIndices = thisCompPins;

                bool hasValidBbox = false;
                if (c.contains("bbox") && c["bbox"].is_object()) {
                    const auto& bb = c["bbox"];
                    pc.minX = bb.value("min_x", bb.value("x1", cx - 20.0));
                    pc.minY = bb.value("min_y", bb.value("y1", normCy - 20.0));
                    pc.maxX = bb.value("max_x", bb.value("x2", cx + 20.0));
                    pc.maxY = bb.value("max_y", bb.value("y2", normCy + 20.0));
                    hasValidBbox = true;
                } else if (c.contains("width") && c["width"].is_number() &&
                           c.contains("height") && c["height"].is_number()) {
                    double w = std::max(0.0, c["width"].get<double>());
                    double h = std::max(0.0, c["height"].get<double>());
                    double rot = std::fmod(std::fabs(c.value("rotation", 0.0)), 180.0);
                    if (std::fabs(rot - 90.0) < 45.0) {
                        std::swap(w, h);
                    }
                    pc.minX = cx - w * 0.5;
                    pc.maxX = cx + w * 0.5;
                    pc.minY = normCy - h * 0.5;
                    pc.maxY = normCy + h * 0.5;
                    hasValidBbox = true;
                } else if (thisCompPins.size() >= 2) {
                    pc.minX = pins[thisCompPins[0]].x;
                    pc.maxX = pins[thisCompPins[0]].x;
                    pc.minY = pins[thisCompPins[0]].normY;
                    pc.maxY = pins[thisCompPins[0]].normY;
                    for (size_t pIdx : thisCompPins) {
                        pc.minX = std::min(pc.minX, pins[pIdx].x);
                        pc.maxX = std::max(pc.maxX, pins[pIdx].x);
                        pc.minY = std::min(pc.minY, pins[pIdx].normY);
                        pc.maxY = std::max(pc.maxY, pins[pIdx].normY);
                    }
                    if (pc.maxX - pc.minX < 10.0) {
                        pc.minX -= 5.0;
                        pc.maxX += 5.0;
                    }
                    if (pc.maxY - pc.minY < 10.0) {
                        pc.minY -= 5.0;
                        pc.maxY += 5.0;
                    }
                    hasValidBbox = true;
                }
                if (hasValidBbox) {
                    partComps.push_back(std::move(pc));
                }
            }
        }
    }

    for (const char* nfArrKey : {"netflags", "labels", "net_labels"}) {
        if (args.contains(nfArrKey) && args[nfArrKey].is_array()) {
            for (const auto& nf : args[nfArrKey]) {
                if (!nf.is_object()) continue;
                std::string nfNet = nf.value("net", nf.value("name", nf.value("value", nf.value("text", ""))));
                if (nfNet.empty()) continue;
                double nx = nf.value("x", 0.0);
                double ny = nf.value("y", 0.0);
                SchPin sp;
                sp.id = nf.value("id", "nf_top_" + std::to_string(pins.size() + 1));
                sp.compId = sp.id;
                sp.ref = nf.value("reference", "#NF");
                sp.number = "1";
                sp.name = nfNet;
                sp.net = nfNet;
                sp.x = nx;
                sp.rawY = ny;
                sp.normY = (ny < 0.0 && anyWireVertex && allWireYNonNegative) ? std::fabs(ny) : ny;
                sp.noConnected = false;
                sp.isPartPin = false;
                sp.isNetFlag = true;
                sp.netFlagName = nfNet;
                pins.push_back(sp);
            }
        }
    }

    size_t nW = wires.size();
    size_t nP = pins.size();
    size_t totalNodes = nW + nP;
    std::vector<size_t> ufParent(totalNodes);
    for (size_t i = 0; i < totalNodes; ++i) ufParent[i] = i;

    auto ufFind = [&](size_t x) {
        size_t r = x;
        while (ufParent[r] != r) r = ufParent[r];
        while (ufParent[x] != r) {
            size_t nxt = ufParent[x];
            ufParent[x] = r;
            x = nxt;
        }
        return r;
    };
    auto ufUnion = [&](size_t a, size_t b) {
        size_t ra = ufFind(a);
        size_t rb = ufFind(b);
        if (ra != rb) ufParent[rb] = ra;
    };

    auto pointTouchesWire = [&](double px, double py, const SchWire& w) -> bool {
        for (size_t k = 0; k + 1 < w.pts.size(); ++k) {
            if (distPointToSeg(px, py, w.pts[k].first, w.pts[k].second,
                               w.pts[k + 1].first, w.pts[k + 1].second) <= kTol) {
                return true;
            }
        }
        return false;
    };

    auto pinMatchesPoint = [&](const SchPin& pin, double px, double py) -> bool {
        if (std::hypot(pin.x - px, pin.normY - py) <= kTol) return true;
        if (std::hypot(pin.x - px, pin.rawY - py) <= kTol) return true;
        return false;
    };

    auto pinMatchesWire = [&](const SchPin& pin, const SchWire& w) -> bool {
        if (pointTouchesWire(pin.x, pin.normY, w)) return true;
        if (std::fabs(pin.normY - pin.rawY) > 1e-6 && pointTouchesWire(pin.x, pin.rawY, w)) {
            return true;
        }
        return false;
    };

    // 1. Wire-to-Wire union (shared vertices, T-junction mid-segment landings, or same primitive ID)
    for (size_t i = 0; i < nW; ++i) {
        for (size_t j = i + 1; j < nW; ++j) {
            bool touches = (!wires[i].id.empty() && wires[i].id == wires[j].id);
            if (!touches) {
                for (const auto& pt : wires[i].pts) {
                    if (pointTouchesWire(pt.first, pt.second, wires[j])) {
                        touches = true;
                        break;
                    }
                }
            }
            if (!touches) {
                for (const auto& pt : wires[j].pts) {
                    if (pointTouchesWire(pt.first, pt.second, wires[i])) {
                        touches = true;
                        break;
                    }
                }
            }
            if (touches) {
                ufUnion(i, j);
            }
        }
    }

    // 2. Pin-to-Wire union
    std::map<std::string, bool> wireTouchesPin;
    for (size_t p = 0; p < nP; ++p) {
        for (size_t w = 0; w < nW; ++w) {
            if (pinMatchesWire(pins[p], wires[w])) {
                ufUnion(nW + p, w);
                pins[p].touchesWire = true;
                wireTouchesPin[wires[w].id] = true;
            }
        }
    }

    // 3. Pin-to-Pin union (colocated pins)
    for (size_t i = 0; i < nP; ++i) {
        for (size_t j = i + 1; j < nP; ++j) {
            if (pinMatchesPoint(pins[i], pins[j].x, pins[j].normY) ||
                pinMatchesPoint(pins[i], pins[j].x, pins[j].rawY)) {
                ufUnion(nW + i, nW + j);
            }
        }
    }

    // Propagate explicit net names & netflags across each DSU component
    struct IslandData {
        std::vector<size_t> wireIndices;
        std::vector<size_t> partPinIndices;
        std::vector<size_t> netFlagPinIndices;
        std::set<std::string> netFlagNames;
        std::set<std::string> explicitNetNames;
        bool hasNamedWire = false;
    };
    std::map<size_t, IslandData> islands;
    for (size_t w = 0; w < nW; ++w) {
        size_t r = ufFind(w);
        islands[r].wireIndices.push_back(w);
        if (!wires[w].net.empty()) {
            islands[r].explicitNetNames.insert(wires[w].net);
            islands[r].hasNamedWire = true;
        }
    }
    for (size_t p = 0; p < nP; ++p) {
        size_t r = ufFind(nW + p);
        if (pins[p].isPartPin) {
            islands[r].partPinIndices.push_back(p);
        }
        if (pins[p].isNetFlag) {
            islands[r].netFlagPinIndices.push_back(p);
            if (!pins[p].netFlagName.empty()) {
                islands[r].netFlagNames.insert(pins[p].netFlagName);
                islands[r].explicitNetNames.insert(pins[p].netFlagName);
            }
        }
        if (!pins[p].net.empty() && !pins[p].noConnected) {
            islands[r].explicitNetNames.insert(pins[p].net);
        }
    }

    // Count global occurrences of each explicit net name across part pins / netflags / named wires
    std::map<std::string, int> globalNetPinCount;
    std::map<std::string, std::vector<size_t>> globalNetPartPins;
    std::map<std::string, int> globalNetFlagCount;
    std::map<std::string, int> globalNetNamedWireCount;
    for (const auto& kv : islands) {
        const auto& isl = kv.second;
        int pinCount = static_cast<int>(isl.partPinIndices.size() + isl.netFlagPinIndices.size());
        for (const auto& netName : isl.explicitNetNames) {
            globalNetPinCount[netName] += pinCount;
            globalNetFlagCount[netName] += static_cast<int>(isl.netFlagPinIndices.size());
            if (isl.hasNamedWire) {
                globalNetNamedWireCount[netName] += 1;
            }
            for (size_t pIdx : isl.partPinIndices) {
                if (!pins[pIdx].noConnected) {
                    globalNetPartPins[netName].push_back(pIdx);
                }
            }
        }
    }

    auto toUpperAscii = [](std::string s) {
        for (char& ch : s) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return s;
    };

    auto classifySupplyRail = [&](const std::string& rawName,
                                  bool& outIsGround,
                                  bool& outIsPositiveSupply,
                                  double& outNominalVolts) {
        outIsGround = false;
        outIsPositiveSupply = false;
        outNominalVolts = -1.0;
        std::string u = toUpperAscii(rawName);
        if (u.empty()) return;

        if (u == "GND" || u == "AGND" || u == "DGND" || u == "PGND" ||
            u == "SGND" || u == "VSS" || u == "AVSS" || u == "DVSS" ||
            u == "0V" || u.rfind("GND_", 0) == 0 ||
            (u.size() >= 4 && u.compare(u.size() - 4, 4, "_GND") == 0)) {
            outIsGround = true;
            outNominalVolts = 0.0;
            return;
        }

        if (u == "VCC" || u == "VDD" || u == "AVCC" || u == "AVDD" ||
            u == "DVCC" || u == "DVDD" || u == "PVCC" || u == "PVDD" ||
            u == "VBUS" || u == "VIN" || u == "VSYS" || u == "VBAT" ||
            u == "VPWR" || u == "V+" || u == "VS") {
            outIsPositiveSupply = true;
            outNominalVolts = -1.0;
            return;
        }

        // Parse explicit voltage rails such as +3V3, 3V3, +3.3V, 3.3V, +5V, 5V, +12V, +1V8, VDD_3V3, VCC_5V
        std::string clean = u;
        for (const char* pfx : {"VDD_", "VCC_", "PWR_", "V_"}) {
            if (clean.rfind(pfx, 0) == 0) {
                clean = clean.substr(std::strlen(pfx));
                break;
            }
        }
        if (!clean.empty() && clean[0] == '+') {
            clean = clean.substr(1);
        }
        // Pattern 1: <int>V<int> e.g. 3V3, 1V8, 2V5
        size_t vPos = clean.find('V');
        if (vPos != std::string::npos && vPos > 0) {
            std::string beforeV = clean.substr(0, vPos);
            std::string afterV = clean.substr(vPos + 1);
            bool beforeDigits = !beforeV.empty();
            for (char c : beforeV) {
                if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') beforeDigits = false;
            }
            bool afterDigits = true;
            for (char c : afterV) {
                if (!std::isdigit(static_cast<unsigned char>(c))) afterDigits = false;
            }
            if (beforeDigits && afterDigits) {
                try {
                    if (afterV.empty()) {
                        outNominalVolts = std::stod(beforeV);
                    } else {
                        outNominalVolts = std::stod(beforeV + "." + afterV);
                    }
                    outIsPositiveSupply = (outNominalVolts > 0.0);
                    return;
                } catch (...) {
                }
            }
        }
    };

    auto isSupplyOrGroundNet = [&](const std::string& netName) -> bool {
        bool isGnd = false;
        bool isPos = false;
        double v = -1.0;
        classifySupplyRail(netName, isGnd, isPos, v);
        return isGnd || isPos;
    };

    auto isPositiveSupplyNet = [&](const std::string& netName) -> bool {
        bool isGnd = false;
        bool isPos = false;
        double v = -1.0;
        classifySupplyRail(netName, isGnd, isPos, v);
        return isPos;
    };

    auto isGroundNet = [&](const std::string& netName) -> bool {
        bool isGnd = false;
        bool isPos = false;
        double v = -1.0;
        classifySupplyRail(netName, isGnd, isPos, v);
        return isGnd;
    };

    const bool isPartialCompSubset = args.value("_partial_component_subset", false);

    // 4. Identify unconnected part pins
    nlohmann::json unconnectedPinsJson = nlohmann::json::array();
    for (size_t p = 0; p < nP; ++p) {
        const auto& pin = pins[p];
        if (!pin.isPartPin || pin.noConnected) continue;
        size_t r = ufFind(nW + p);
        const auto& isl = islands[r];
        size_t totalIslandPins = isl.partPinIndices.size() + isl.netFlagPinIndices.size();
        bool connectedByNetName = isl.hasNamedWire || (isPartialCompSubset && pin.touchesWire);
        if (!connectedByNetName) {
            for (const auto& netName : isl.explicitNetNames) {
                if (!netName.empty() && (globalNetPinCount[netName] >= 2 ||
                                         !isl.netFlagPinIndices.empty() ||
                                         isSupplyOrGroundNet(netName) ||
                                         isPartialCompSubset)) {
                    connectedByNetName = true;
                    break;
                }
            }
        }
        if (totalIslandPins < 2 && !connectedByNetName) {
            unconnectedPinsJson.push_back({
                {"id", pin.id},
                {"reference", pin.ref},
                {"pin_number", pin.number},
                {"pin_name", pin.name},
                {"x", pin.x},
                {"y", pin.normY},
                {"reason", pin.touchesWire ? "single_pin_net" : "unconnected_pin"}
            });
        }
    }

    // 5. Identify dangling wires
    nlohmann::json danglingWiresJson = nlohmann::json::array();
    for (size_t w = 0; !isPartialCompSubset && w < nW; ++w) {
        const auto& wire = wires[w];
        size_t r = ufFind(w);
        const auto& isl = islands[r];
        size_t totalIslandPins = isl.partPinIndices.size() + isl.netFlagPinIndices.size();

        std::pair<double, double> eps[2] = {wire.pts.front(), wire.pts.back()};
        int anchoredEps = 0;
        std::pair<double, double> unanchoredPt = eps[0];

        for (int ep = 0; ep < 2; ++ep) {
            double ex = eps[ep].first;
            double ey = eps[ep].second;
            bool anchored = false;
            for (size_t p = 0; p < nP; ++p) {
                if (pinMatchesPoint(pins[p], ex, ey)) {
                    anchored = true;
                    break;
                }
            }
            if (!anchored) {
                for (size_t otherW = 0; otherW < nW; ++otherW) {
                    if (otherW == w) continue;
                    if (pointTouchesWire(ex, ey, wires[otherW])) {
                        anchored = true;
                        break;
                    }
                }
            }
            if (anchored) {
                ++anchoredEps;
            } else {
                unanchoredPt = {ex, ey};
            }
        }

        bool isNamedNetStub = !wire.net.empty() && wireTouchesPin[wire.id] && anchoredEps >= 1;
        if ((totalIslandPins < 2 && !isNamedNetStub) || (anchoredEps < 2 && !isNamedNetStub)) {
            danglingWiresJson.push_back({
                {"wire_id", wire.id},
                {"net", wire.net},
                {"x", unanchoredPt.first},
                {"y", unanchoredPt.second},
                {"reason", (totalIslandPins == 0) ? "isolated_wire" : "unanchored_wire_endpoint"}
            });
        }
    }

    // 6. Net issues (shorted netflags / conflicting net names), voltage domain issues, and inferred nets
    nlohmann::json netIssuesJson = nlohmann::json::array();
    nlohmann::json voltageDomainIssuesJson = nlohmann::json::array();
    nlohmann::json inferredNetsJson = nlohmann::json::array();
    int powerShortsCount = 0;
    int autoNetIdx = 1;
    std::vector<std::string> pinResolvedNet(nP, "");

    std::map<std::string, nlohmann::json> mergedInferredNets;
    std::vector<std::string> inferredNetOrder;

    for (const auto& kv : islands) {
        const auto& isl = kv.second;
        if (isl.partPinIndices.empty() && isl.netFlagPinIndices.empty()) continue;

        nlohmann::json pinRefsJson = nlohmann::json::array();
        for (size_t pIdx : isl.partPinIndices) {
            pinRefsJson.push_back(pins[pIdx].ref + "." + pins[pIdx].number);
        }

        if (isl.netFlagNames.size() > 1 || isl.explicitNetNames.size() > 1) {
            std::string joined;
            const auto& nameSet = (isl.netFlagNames.size() > 1) ? isl.netFlagNames : isl.explicitNetNames;
            nlohmann::json railsArr = nlohmann::json::array();
            int supplyRailCount = 0;
            for (const auto& nm : nameSet) {
                if (!joined.empty()) joined += "/";
                joined += nm;
                if (isSupplyOrGroundNet(nm)) {
                    ++supplyRailCount;
                    railsArr.push_back(nm);
                }
            }
            ++powerShortsCount;
            netIssuesJson.push_back({
                {"net", joined},
                {"issue", "shorted_nets"},
                {"pins", pinRefsJson}
            });
            if (supplyRailCount >= 2) {
                voltageDomainIssuesJson.push_back({
                    {"net", joined},
                    {"issue", "supply_rail_conflict"},
                    {"rails", railsArr},
                    {"pins", pinRefsJson}
                });
            }
        }

        std::string resolvedNet;
        if (!isl.netFlagNames.empty()) {
            resolvedNet = *isl.netFlagNames.begin();
        } else if (!isl.explicitNetNames.empty()) {
            resolvedNet = *isl.explicitNetNames.begin();
        } else if (!isl.partPinIndices.empty()) {
            resolvedNet = "$NET_" + std::to_string(autoNetIdx++);
        }

        for (size_t pIdx : isl.partPinIndices) {
            if (!pins[pIdx].noConnected) {
                pinResolvedNet[pIdx] = !pins[pIdx].net.empty() ? pins[pIdx].net : resolvedNet;
            }
        }

        if (!isl.partPinIndices.empty() && !resolvedNet.empty()) {
            auto it = mergedInferredNets.find(resolvedNet);
            if (it == mergedInferredNets.end()) {
                inferredNetOrder.push_back(resolvedNet);
                mergedInferredNets[resolvedNet] = pinRefsJson;
            } else {
                for (const auto& pr : pinRefsJson) {
                    it->second.push_back(pr);
                }
            }
        }
    }

    for (const auto& nName : inferredNetOrder) {
        inferredNetsJson.push_back({
            {"name", nName},
            {"pins", mergedInferredNets[nName]}
        });
    }

    // 7. Semantic Layer 2a: Single-pin signal nets across the schematic
    nlohmann::json singlePinNetsJson = nlohmann::json::array();
    if (!isPartialCompSubset) {
        for (const auto& kv : globalNetPartPins) {
            const std::string& netName = kv.first;
            const auto& pList = kv.second;
            if (netName.empty() || netName.rfind("$NET_", 0) == 0) continue;
            if (isSupplyOrGroundNet(netName)) continue;
            if (globalNetFlagCount[netName] > 0) continue;
            if (isMultiSheetSchematic && globalNetNamedWireCount[netName] > 0) continue;
            if (pList.size() == 1) {
                const auto& sp = pins[pList[0]];
                singlePinNetsJson.push_back({
                    {"net", netName},
                    {"reference", sp.ref},
                    {"pin_number", sp.number},
                    {"pin_name", sp.name},
                    {"x", sp.x},
                    {"y", sp.normY},
                    {"pin", {
                        {"id", sp.id},
                        {"reference", sp.ref},
                        {"pin_number", sp.number},
                        {"pin_name", sp.name},
                        {"x", sp.x},
                        {"y", sp.normY}
                    }}
                });
            }
        }
    }

    // 8. Semantic Layer 2b: Overlapping component bounding boxes / pin-in-foreign-bbox collisions
    nlohmann::json overlappingComponentsJson = nlohmann::json::array();
    for (size_t i = 0; i < partComps.size(); ++i) {
        for (size_t j = i + 1; j < partComps.size(); ++j) {
            const auto& a = partComps[i];
            const auto& b = partComps[j];
            double overlapX = std::min(a.maxX, b.maxX) - std::max(a.minX, b.minX);
            double overlapY = std::min(a.maxY, b.maxY) - std::max(a.minY, b.minY);
            if (overlapX > kTol && overlapY > kTol) {
                bool pinInside = false;
                for (size_t pIdx : a.pinIndices) {
                    if (pins[pIdx].x > b.minX + kTol && pins[pIdx].x < b.maxX - kTol &&
                        pins[pIdx].normY > b.minY + kTol && pins[pIdx].normY < b.maxY - kTol) {
                        pinInside = true;
                        break;
                    }
                }
                if (!pinInside) {
                    for (size_t pIdx : b.pinIndices) {
                        if (pins[pIdx].x > a.minX + kTol && pins[pIdx].x < a.maxX - kTol &&
                            pins[pIdx].normY > a.minY + kTol && pins[pIdx].normY < a.maxY - kTol) {
                            pinInside = true;
                            break;
                        }
                    }
                }
                overlappingComponentsJson.push_back({
                    {"id_a", a.id},
                    {"id_b", b.id},
                    {"reference_a", a.ref},
                    {"reference_b", b.ref},
                    {"overlap_type", pinInside ? "pin_in_foreign_bbox" : "bbox_intersection"},
                    {"bbox_a", {{"min_x", a.minX}, {"min_y", a.minY}, {"max_x", a.maxX}, {"max_y", a.maxY}}},
                    {"bbox_b", {{"min_x", b.minX}, {"min_y", b.minY}, {"max_x", b.maxX}, {"max_y", b.maxY}}}
                });
            }
        }
    }

    // Group all connected part pins by resolved net name
    std::map<std::string, std::vector<size_t>> netToPartPins;
    for (size_t p = 0; p < nP; ++p) {
        if (!pins[p].isPartPin || pins[p].noConnected) continue;
        if (!pinResolvedNet[p].empty()) {
            netToPartPins[pinResolvedNet[p]].push_back(p);
        }
    }

    auto hasRefPrefix = [&](const std::string& ref, const char* pfx) -> bool {
        std::string uRef = toUpperAscii(ref);
        std::string uPfx = toUpperAscii(pfx);
        if (uRef.rfind(uPfx, 0) != 0) return false;
        if (uRef.size() == uPfx.size()) return true;
        char nextCh = uRef[uPfx.size()];
        return std::isdigit(static_cast<unsigned char>(nextCh)) || nextCh == '?' || nextCh == '_';
    };

    // Identify nets that have a pull-up resistor (R*, RN*, RP*, RB*, RA*) to a positive supply rail
    // and positive supply rails that have a decoupling capacitor (C*) to ground
    std::set<std::string> pulledUpNets;
    std::set<std::string> decoupledSupplyNets;
    for (const auto& pc : partComps) {
        bool isResistor = hasRefPrefix(pc.ref, "R") || hasRefPrefix(pc.ref, "RN") ||
                          hasRefPrefix(pc.ref, "RP") || hasRefPrefix(pc.ref, "RB") ||
                          hasRefPrefix(pc.ref, "RA");
        bool isCapacitor = hasRefPrefix(pc.ref, "C");
        if (!isResistor && !isCapacitor) continue;

        std::vector<std::string> compNets;
        for (size_t pIdx : pc.pinIndices) {
            if (!pins[pIdx].noConnected && !pinResolvedNet[pIdx].empty()) {
                compNets.push_back(pinResolvedNet[pIdx]);
            }
        }
        if (isResistor) {
            bool touchesPosSupply = false;
            for (const auto& n : compNets) {
                if (isPositiveSupplyNet(n)) {
                    touchesPosSupply = true;
                    break;
                }
            }
            if (touchesPosSupply) {
                for (const auto& n : compNets) {
                    if (!isSupplyOrGroundNet(n)) {
                        pulledUpNets.insert(n);
                    }
                }
            }
        }
        if (isCapacitor) {
            bool touchesGnd = false;
            for (const auto& n : compNets) {
                if (isGroundNet(n)) {
                    touchesGnd = true;
                    break;
                }
            }
            if (touchesGnd) {
                for (const auto& n : compNets) {
                    if (isPositiveSupplyNet(n)) {
                        decoupledSupplyNets.insert(n);
                    }
                }
            }
        }
    }

    // 9. Semantic Layer 3: Pull-up resistor validation on open-drain / I2C / active-low reset nets
    auto requiresPullup = [&](const std::string& netName) -> bool {
        if (netName.empty() || netName.rfind("$NET_", 0) == 0 || isSupplyOrGroundNet(netName)) {
            return false;
        }
        std::string u = toUpperAscii(netName);
        if (u == "SDA" || u == "SCL" || u == "NRST" || u == "NRESET" || u == "RESET_N" || u == "RST_N") {
            return true;
        }
        for (const char* tok : {"SDA", "SCL", "I2C", "SMBUS", "ALERT", "FAULT", "NRST", "NRESET", "RESET_N", "RST_N"}) {
            if (u.find(tok) != std::string::npos) {
                return true;
            }
        }
        return false;
    };

    nlohmann::json pullupIssuesJson = nlohmann::json::array();
    if (!isPartialCompSubset) {
        for (const auto& kv : netToPartPins) {
            const std::string& netName = kv.first;
            const auto& pList = kv.second;
            if (pList.empty()) continue;
            if (requiresPullup(netName) && pulledUpNets.count(netName) == 0) {
                nlohmann::json netPinRefs = nlohmann::json::array();
                for (size_t pIdx : pList) {
                    netPinRefs.push_back(pins[pIdx].ref + "." + pins[pIdx].number);
                }
                pullupIssuesJson.push_back({
                    {"net", netName},
                    {"issue", "missing_pullup_resistor"},
                    {"pins", netPinRefs}
                });
            }
        }
    }

    // 10. Semantic Layer 4: Decoupling capacitor validation on active ICs (U* / IC*)
    auto isSupplyPinName = [&](const std::string& pName) -> bool {
        std::string u = toUpperAscii(pName);
        if (u.empty()) return false;
        if (u == "VCC" || u == "VDD" || u == "AVCC" || u == "DVCC" ||
            u == "AVDD" || u == "DVDD" || u == "VDDIO" || u == "VDDA" ||
            u == "VDDD" || u == "VBAT" || u == "VS" || u == "V+" ||
            u == "VIN" || u == "VPWR" || u == "PWR") {
            return true;
        }
        if (u.rfind("VCC", 0) == 0 || u.rfind("VDD", 0) == 0 ||
            u.rfind("AVCC", 0) == 0 || u.rfind("DVCC", 0) == 0 ||
            u.rfind("AVDD", 0) == 0 || u.rfind("DVDD", 0) == 0) {
            return true;
        }
        bool isGnd = false;
        bool isPos = false;
        double v = -1.0;
        classifySupplyRail(u, isGnd, isPos, v);
        return isPos;
    };

    nlohmann::json decouplingIssuesJson = nlohmann::json::array();
    if (!isPartialCompSubset) {
        for (const auto& pc : partComps) {
            if (!hasRefPrefix(pc.ref, "U") && !hasRefPrefix(pc.ref, "IC")) continue;
            std::set<std::string> allPosSupplyNets;
            std::set<std::string> namedSupplyPinNets;
            std::string icGndNet;
            for (size_t pIdx : pc.pinIndices) {
                if (pins[pIdx].noConnected) continue;
                const std::string& n = pinResolvedNet[pIdx];
                if (n.empty()) continue;
                if (isGroundNet(n)) {
                    icGndNet = n;
                } else if (isPositiveSupplyNet(n)) {
                    allPosSupplyNets.insert(n);
                    if (isSupplyPinName(pins[pIdx].name)) {
                        namedSupplyPinNets.insert(n);
                    }
                }
            }
            const std::set<std::string>& icSupplyNets =
                !namedSupplyPinNets.empty() ? namedSupplyPinNets : allPosSupplyNets;
            if (!icGndNet.empty() && !icSupplyNets.empty()) {
                for (const auto& supNet : icSupplyNets) {
                    if (decoupledSupplyNets.count(supNet) == 0) {
                        decouplingIssuesJson.push_back({
                            {"id", pc.id},
                            {"reference", pc.ref},
                            {"supply_net", supNet},
                            {"ground_net", icGndNet},
                            {"issue", "missing_decoupling_capacitor"},
                            {"x", pc.x},
                            {"y", pc.normY}
                        });
                    }
                }
            }
        }
    }

    int unconnectedCount = static_cast<int>(unconnectedPinsJson.size());
    int danglingWiresCount = static_cast<int>(danglingWiresJson.size());
    int compIssuesCount = static_cast<int>(componentIssuesJson.size());
    int netIssuesCount = static_cast<int>(netIssuesJson.size());
    int singlePinNetsCount = static_cast<int>(singlePinNetsJson.size());
    int overlappingComponentsCount = static_cast<int>(overlappingComponentsJson.size());
    int pullupIssuesCount = static_cast<int>(pullupIssuesJson.size());
    int decouplingIssuesCount = static_cast<int>(decouplingIssuesJson.size());
    int voltageDomainIssuesCount = static_cast<int>(voltageDomainIssuesJson.size());
    int inferredNetsCount = static_cast<int>(inferredNetsJson.size());

    bool passed = (unconnectedCount == 0 && danglingWiresCount == 0 &&
                   compIssuesCount == 0 && powerShortsCount == 0 && netIssuesCount == 0 &&
                   singlePinNetsCount == 0 && overlappingComponentsCount == 0 &&
                   pullupIssuesCount == 0 && decouplingIssuesCount == 0 &&
                   voltageDomainIssuesCount == 0);

    outSuccess = true;
    outSummary = std::string("Schematic ERC ") + (passed ? "PASSED" : "FAILED") +
                 " • " + std::to_string(unconnectedCount) + " unconnected pins • " +
                 std::to_string(danglingWiresCount) + " dangling wires • " +
                 std::to_string(compIssuesCount) + " component issues • " +
                 std::to_string(netIssuesCount) + " net issues • " +
                 std::to_string(singlePinNetsCount) + " single-pin nets • " +
                 std::to_string(overlappingComponentsCount) + " overlaps • " +
                 std::to_string(pullupIssuesCount) + " pull-up issues • " +
                 std::to_string(decouplingIssuesCount) + " decoupling issues • " +
                 std::to_string(voltageDomainIssuesCount) + " voltage domain issues";

    nlohmann::json res = {
        {"ok", true},
        {"passed", passed},
        {"total_part_components", totalPartComponents},
        {"total_pins", totalPartPins},
        {"unconnected_pins_count", unconnectedCount},
        {"unconnected_pins", unconnectedPinsJson},
        {"dangling_wires_count", danglingWiresCount},
        {"dangling_wires", danglingWiresJson},
        {"component_issues_count", compIssuesCount},
        {"component_issues", componentIssuesJson},
        {"net_issues_count", netIssuesCount},
        {"net_issues", netIssuesJson},
        {"single_pin_nets_count", singlePinNetsCount},
        {"single_pin_nets", singlePinNetsJson},
        {"overlapping_components_count", overlappingComponentsCount},
        {"overlapping_components", overlappingComponentsJson},
        {"pullup_issues_count", pullupIssuesCount},
        {"pullup_issues", pullupIssuesJson},
        {"decoupling_issues_count", decouplingIssuesCount},
        {"decoupling_issues", decouplingIssuesJson},
        {"voltage_domain_issues_count", voltageDomainIssuesCount},
        {"voltage_domain_issues", voltageDomainIssuesJson},
        {"inferred_nets_count", inferredNetsCount},
        {"inferred_nets", inferredNetsJson},
        {"summary", outSummary}
    };
    attachProvenance(res, "eda_calc_schematic_erc_audit");
    return res;
}

} // namespace aimon

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
