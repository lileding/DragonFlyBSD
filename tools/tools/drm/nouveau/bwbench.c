/*
 * bwbench - standalone NVK/Vulkan memory-read-bandwidth microbenchmark.
 *
 * Allocates a large device-local buffer (>> L2), runs a grid-stride
 * read-accumulate compute shader for several timed submits, and reports the
 * achieved DRAM read bandwidth in GB/s. Purpose: decide whether the llama.cpp
 * decode (tg) shortfall is a caller-side / NVK-codegen issue or a real
 * driver/memory-clock limit. If this hits a high fraction of the 2080 Ti's
 * 616 GB/s peak, the memory subsystem is fine and the tg gap is caller-side.
 *
 * Build:  cc -O2 -o bwbench bwbench.c -lvulkan
 * Run:    VK_ICD_FILENAMES=.../nouveau_icd.json ./bwbench [buf_MB] [iters] [reps]
 */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
	fprintf(stderr, "%s failed: %d (line %d)\n", #x, _r, __LINE__); \
	exit(1); } } while (0)

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static uint32_t *load_spv(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { perror(path); exit(1); }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint32_t *buf = malloc(sz);
	if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "read spv\n"); exit(1); }
	fclose(f);
	*len = sz;
	return buf;
}

static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
		if ((bits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & want) == want)
			return i;
	fprintf(stderr, "no mem type for 0x%x\n", want);
	exit(1);
}

static void mkbuf(VkDevice dev, VkPhysicalDevice pd, VkDeviceSize sz,
    VkBuffer *buf, VkDeviceMemory *mem)
{
	VkBufferCreateInfo bi = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = sz,
		.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	CK(vkCreateBuffer(dev, &bi, NULL, buf));
	VkMemoryRequirements mr;
	vkGetBufferMemoryRequirements(dev, *buf, &mr);
	VkMemoryAllocateInfo ai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = find_mem_type(pd, mr.memoryTypeBits,
		    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
	};
	CK(vkAllocateMemory(dev, &ai, NULL, mem));
	CK(vkBindBufferMemory(dev, *buf, *mem, 0));
}

int main(int argc, char **argv)
{
	uint32_t buf_mb = argc > 1 ? atoi(argv[1]) : 256;
	uint32_t iters  = argc > 2 ? atoi(argv[2]) : 8;   /* inner re-reads */
	uint32_t reps   = argc > 3 ? atoi(argv[3]) : 30;  /* timed submits */
	uint32_t groups = argc > 4 ? atoi(argv[4]) : 4096; /* workgroups */

	VkDeviceSize src_sz = (VkDeviceSize)buf_mb * 1024 * 1024;
	uint32_t n4 = (uint32_t)(src_sz / 16);            /* vec4 count */
	VkDeviceSize dst_sz = (VkDeviceSize)groups * 256 * 16;

	/* instance */
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_1 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app };
	VkInstance inst;
	CK(vkCreateInstance(&ici, NULL, &inst));

	uint32_t ndev = 0;
	CK(vkEnumeratePhysicalDevices(inst, &ndev, NULL));
	if (!ndev) { fprintf(stderr, "no vulkan device\n"); return 1; }
	VkPhysicalDevice pds[8];
	if (ndev > 8) ndev = 8;
	CK(vkEnumeratePhysicalDevices(inst, &ndev, pds));
	VkPhysicalDevice pd = pds[0];
	VkPhysicalDeviceProperties pp;
	vkGetPhysicalDeviceProperties(pd, &pp);

	/* compute queue family */
	uint32_t nqf = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
	VkQueueFamilyProperties qf[16];
	if (nqf > 16) nqf = 16;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qf);
	uint32_t qfi = UINT32_MAX;
	for (uint32_t i = 0; i < nqf; i++)
		if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }
	if (qfi == UINT32_MAX) { fprintf(stderr, "no compute queue\n"); return 1; }

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
	VkDevice dev;
	CK(vkCreateDevice(pd, &dci, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qfi, 0, &queue);

	VkBuffer src, dst;
	VkDeviceMemory srcm, dstm;
	mkbuf(dev, pd, src_sz, &src, &srcm);
	mkbuf(dev, pd, dst_sz, &dst, &dstm);

	/* shader module */
	size_t spvlen;
	uint32_t *spv = load_spv("bw.spv", &spvlen);
	VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = spvlen, .pCode = spv };
	VkShaderModule sm;
	CK(vkCreateShaderModule(dev, &smi, NULL, &sm));

	/* descriptor set layout: 2 storage buffers */
	VkDescriptorSetLayoutBinding b[2] = {
		{ .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
		{ .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		  .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT },
	};
	VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2, .pBindings = b };
	VkDescriptorSetLayout dsl;
	CK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));

	VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
		.offset = 0, .size = 8 };
	VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1, .pSetLayouts = &dsl,
		.pushConstantRangeCount = 1, .pPushConstantRanges = &pcr };
	VkPipelineLayout pl;
	CK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));

	VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
		.layout = pl };
	VkPipeline pipe;
	CK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

	/* descriptor pool + set */
	VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
	VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps };
	VkDescriptorPool dp;
	CK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
	VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
	VkDescriptorSet ds;
	CK(vkAllocateDescriptorSets(dev, &dsai, &ds));
	VkDescriptorBufferInfo bis[2] = {
		{ .buffer = src, .offset = 0, .range = VK_WHOLE_SIZE },
		{ .buffer = dst, .offset = 0, .range = VK_WHOLE_SIZE },
	};
	VkWriteDescriptorSet w[2] = {
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bis[0] },
		{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 1,
		  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bis[1] },
	};
	vkUpdateDescriptorSets(dev, 2, w, 0, NULL);

	/* command buffer */
	VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = qfi };
	VkCommandPool cp;
	CK(vkCreateCommandPool(dev, &cpci, NULL, &cp));
	VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = cp, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
	VkCommandBuffer cb;
	CK(vkAllocateCommandBuffers(dev, &cbai, &cb));

	uint32_t pc[2] = { n4, iters };
	VkCommandBufferBeginInfo bbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CK(vkBeginCommandBuffer(cb, &bbi));
	vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
	vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
	vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, pc);
	vkCmdDispatch(cb, groups, 1, 1);
	CK(vkEndCommandBuffer(cb));

	VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1, .pCommandBuffers = &cb };

	printf("device: %s\n", pp.deviceName);
	printf("buffer: %u MB (%u vec4), iters=%u groups=%u reps=%u\n",
	    buf_mb, n4, iters, groups, reps);

	/* warmup */
	CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));

	double t0 = now_s();
	for (uint32_t r = 0; r < reps; r++)
		CK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
	CK(vkQueueWaitIdle(queue));
	double dt = now_s() - t0;

	double bytes = (double)src_sz * (double)iters * (double)reps;
	double gbps = bytes / dt / 1e9;
	printf("read %.2f GB in %.4f s => %.1f GB/s  (%.1f%% of 616 GB/s peak)\n",
	    bytes / 1e9, dt, gbps, gbps / 616.0 * 100.0);
	return 0;
}
