/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
*/
#include <stdio.h>

#include "CUnit/Basic.h"

#include "util_math.h"

#include "amdgpu_test.h"
#include "uvd_messages.h"
#include "amdgpu_drm.h"
#include "amdgpu_internal.h"

#define IB_SIZE		amdgpu_cs_ib_size_4K
#define MAX_RESOURCES	16

static amdgpu_device_handle device_handle;
static uint32_t major_version;
static uint32_t minor_version;
static uint32_t family_id;

static amdgpu_context_handle context_handle;
static amdgpu_ib_handle ib_handle;
uint32_t *ib_cpu;

static amdgpu_bo_handle resources[MAX_RESOURCES];
static unsigned num_resources;

static void amdgpu_cs_uvd_create(void);
static void amdgpu_cs_uvd_decode(void);
static void amdgpu_cs_uvd_destroy(void);

CU_TestInfo cs_tests[] = {
	{ "UVD create",  amdgpu_cs_uvd_create },
	{ "UVD decode",  amdgpu_cs_uvd_decode },
	{ "UVD destroy",  amdgpu_cs_uvd_destroy },
	CU_TEST_INFO_NULL,
};

int suite_cs_tests_init(void)
{
	struct amdgpu_cs_ib_alloc_result ib_result = {0};
	int r;

	r = amdgpu_device_initialize(drm_amdgpu[0], &major_version,
				     &minor_version, &device_handle);
	if (r)
		return CUE_SINIT_FAILED;

	family_id = device_handle->info.family_id;

	r = amdgpu_cs_ctx_create(device_handle, &context_handle);
	if (r)
		return CUE_SINIT_FAILED;

        r = amdgpu_cs_alloc_ib(context_handle, IB_SIZE, &ib_result);
	if (r)
		return CUE_SINIT_FAILED;

	ib_handle = ib_result.handle;
	ib_cpu = ib_result.cpu;

	return CUE_SUCCESS;
}

int suite_cs_tests_clean(void)
{
	int r;

	r = amdgpu_cs_free_ib(ib_handle);
	if (r)
		return CUE_SCLEAN_FAILED;

	r = amdgpu_cs_ctx_free(context_handle);
	if (r)
		return CUE_SCLEAN_FAILED;

	r = amdgpu_device_deinitialize(device_handle);
	if (r)
		return CUE_SCLEAN_FAILED;

	return CUE_SUCCESS;
}

static int submit(unsigned ndw, unsigned ip)
{
	struct amdgpu_cs_ib_alloc_result ib_result = {0};
	struct amdgpu_cs_request ibs_request = {0};
	struct amdgpu_cs_ib_info ib_info = {0};
	struct amdgpu_cs_query_fence fence_status = {0};
	uint32_t expired;
	int r;

	ib_info.ib_handle = ib_handle;
	ib_info.size = ndw;

	ibs_request.ip_type = ip;

	r = amdgpu_bo_list_create(device_handle, num_resources, resources,
				  NULL, &ibs_request.resources);
	if (r)
		return r;

	ibs_request.number_of_ibs = 1;
	ibs_request.ibs = &ib_info;

	r = amdgpu_cs_submit(context_handle, 0,
			     &ibs_request, 1, &fence_status.fence);
	if (r)
		return r;

	r = amdgpu_bo_list_destroy(ibs_request.resources);
	if (r)
		return r;

	fence_status.context = context_handle;
	fence_status.timeout_ns = AMDGPU_TIMEOUT_INFINITE;
	fence_status.ip_type = ip;

	r = amdgpu_cs_query_fence_status(&fence_status, &expired);
	if (r)
		return r;

	return 0;
}

static void uvd_cmd(uint64_t addr, unsigned cmd, int *idx)
{
	ib_cpu[(*idx)++] = 0x3BC4;
	ib_cpu[(*idx)++] = addr;
	ib_cpu[(*idx)++] = 0x3BC5;
	ib_cpu[(*idx)++] = addr >> 32;
	ib_cpu[(*idx)++] = 0x3BC3;
	ib_cpu[(*idx)++] = cmd << 1;
}

static void amdgpu_cs_uvd_create(void)
{
	struct amdgpu_bo_alloc_request req = {0};
	struct amdgpu_bo_alloc_result res = {0};
	void *msg;
	int i, r;

	req.alloc_size = 4*1024;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

	r = amdgpu_bo_alloc(device_handle, &req, &res);
	CU_ASSERT_EQUAL(r, 0);

	r = amdgpu_bo_cpu_map(res.buf_handle, &msg);
	CU_ASSERT_EQUAL(r, 0);

	memcpy(msg, uvd_create_msg, sizeof(uvd_create_msg));
	if (family_id >= AMDGPU_FAMILY_VI)
		((uint8_t*)msg)[0x10] = 7;

	r = amdgpu_bo_cpu_unmap(res.buf_handle);
	CU_ASSERT_EQUAL(r, 0);

	num_resources = 0;
	resources[num_resources++] = res.buf_handle;

	i = 0;
	uvd_cmd(res.virtual_mc_base_address, 0x0, &i);
	for (; i % 16; ++i)
		ib_cpu[i] = 0x80000000;

	r = submit(i, AMDGPU_HW_IP_UVD);
	CU_ASSERT_EQUAL(r, 0);

	r = amdgpu_bo_free(resources[0]);
	CU_ASSERT_EQUAL(r, 0);
}

static void amdgpu_cs_uvd_decode(void)
{
	const unsigned dpb_size = 15923584, dt_size = 737280;
	uint64_t msg_addr, fb_addr, bs_addr, dpb_addr, dt_addr, it_addr;
	struct amdgpu_bo_alloc_request req = {0};
	struct amdgpu_bo_alloc_result res = {0};
	uint64_t sum;
	uint8_t *ptr;
	int i, r;

	req.alloc_size = 4*1024; /* msg */
	req.alloc_size += 4*1024; /* fb */
	if (family_id >= AMDGPU_FAMILY_VI)
		req.alloc_size += 4096; /*it_scaling_table*/
	req.alloc_size += ALIGN(sizeof(uvd_bitstream), 4*1024);
	req.alloc_size += ALIGN(dpb_size, 4*1024);
	req.alloc_size += ALIGN(dt_size, 4*1024);

	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

	r = amdgpu_bo_alloc(device_handle, &req, &res);
	CU_ASSERT_EQUAL(r, 0);

	r = amdgpu_bo_cpu_map(res.buf_handle, (void **)&ptr);
	CU_ASSERT_EQUAL(r, 0);

	memcpy(ptr, uvd_decode_msg, sizeof(uvd_create_msg));
	if (family_id >= AMDGPU_FAMILY_VI)
		ptr[0x10] = 7;

	ptr += 4*1024;
	memset(ptr, 0, 4*1024);
	if (family_id >= AMDGPU_FAMILY_VI) {
		ptr += 4*1024;
		memcpy(ptr, uvd_it_scaling_table, sizeof(uvd_it_scaling_table));
	}

	ptr += 4*1024;
	memcpy(ptr, uvd_bitstream, sizeof(uvd_bitstream));

	ptr += ALIGN(sizeof(uvd_bitstream), 4*1024);
	memset(ptr, 0, dpb_size);

	ptr += ALIGN(dpb_size, 4*1024);
	memset(ptr, 0, dt_size);

	num_resources = 0;
	resources[num_resources++] = res.buf_handle;

	msg_addr = res.virtual_mc_base_address;
	fb_addr = msg_addr + 4*1024;
	if (family_id >= AMDGPU_FAMILY_VI) {
		it_addr = fb_addr + 4*1024;
		bs_addr = it_addr + 4*1024;
	} else
		bs_addr = fb_addr + 4*1024;
	dpb_addr = ALIGN(bs_addr + sizeof(uvd_bitstream), 4*1024);
	dt_addr = ALIGN(dpb_addr + dpb_size, 4*1024);

	i = 0;
	uvd_cmd(msg_addr, 0x0, &i);
	uvd_cmd(dpb_addr, 0x1, &i);
	uvd_cmd(dt_addr, 0x2, &i);
	uvd_cmd(fb_addr, 0x3, &i);
	uvd_cmd(bs_addr, 0x100, &i);
	if (family_id >= AMDGPU_FAMILY_VI)
		uvd_cmd(it_addr, 0x204, &i);
	ib_cpu[i++] = 0x3BC6;
	ib_cpu[i++] = 0x1;
	for (; i % 16; ++i)
		ib_cpu[i] = 0x80000000;

	r = submit(i, AMDGPU_HW_IP_UVD);
	CU_ASSERT_EQUAL(r, 0);

	/* TODO: use a real CRC32 */
	for (i = 0, sum = 0; i < dt_size; ++i)
		sum += ptr[i];
	CU_ASSERT_EQUAL(sum, 0x20345d8);

	r = amdgpu_bo_cpu_unmap(res.buf_handle);
	CU_ASSERT_EQUAL(r, 0);

	r = amdgpu_bo_free(resources[0]);
	CU_ASSERT_EQUAL(r, 0);
}

static void amdgpu_cs_uvd_destroy(void)
{
	struct amdgpu_bo_alloc_request req = {0};
	struct amdgpu_bo_alloc_result res = {0};
	void *msg;
	int i, r;

	req.alloc_size = 4*1024;
	req.preferred_heap = AMDGPU_GEM_DOMAIN_GTT;

	r = amdgpu_bo_alloc(device_handle, &req, &res);
	CU_ASSERT_EQUAL(r, 0);

	r = amdgpu_bo_cpu_map(res.buf_handle, &msg);
	CU_ASSERT_EQUAL(r, 0);

	memcpy(msg, uvd_destroy_msg, sizeof(uvd_create_msg));
	if (family_id >= AMDGPU_FAMILY_VI)
		((uint8_t*)msg)[0x10] = 7;

	r = amdgpu_bo_cpu_unmap(res.buf_handle);
	CU_ASSERT_EQUAL(r, 0);

	num_resources = 0;
	resources[num_resources++] = res.buf_handle;

	i = 0;
	uvd_cmd(res.virtual_mc_base_address, 0x0, &i);
	for (; i % 16; ++i)
		ib_cpu[i] = 0x80000000;

	r = submit(i, AMDGPU_HW_IP_UVD);
	CU_ASSERT_EQUAL(r, 0);

	r = amdgpu_bo_free(resources[0]);
	CU_ASSERT_EQUAL(r, 0);
}