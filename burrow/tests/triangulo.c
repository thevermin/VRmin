// Burrow's GPU drawing test: draws a triangle with Vulkan into an image (no window), reads it back and checks a few
// pixels: the clear color in a corner and the triangle's color in the middle. It also saves the image as
// triangulo.ppm. Natively on Linux it runs on Mesa's CPU driver (lavapipe); under Burrow, on the Mac's GPU.
// The shaders are SPIR-V from triangulo.vert and triangulo.frag (glslangValidator, see burrow.yml).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "triangulo_vert.h"
#include "triangulo_frag.h"

#define W 256
#define H 256
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("FAIL %s: %d\n", #x, r_); return 1; } } while (0)

static uint32_t memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  return UINT32_MAX;
}

int main(void) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "burrow-triangulo", 1, NULL, 0, VK_API_VERSION_1_1};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
  VkInstance inst;
  CHECK(vkCreateInstance(&ici, NULL, &inst));
  uint32_t n = 1;
  VkPhysicalDevice pd;
  VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
  if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) { printf("FAIL no GPU\n"); return 1; }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(pd, &props);
  printf("GPU: %s\n", props.deviceName);

  uint32_t nq = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
  VkQueueFamilyProperties qf[16];
  if (nq > 16) nq = 16;
  vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
  uint32_t fam = UINT32_MAX;
  for (uint32_t i = 0; i < nq && fam == UINT32_MAX; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) fam = i;
  if (fam == UINT32_MAX) { printf("FAIL no graphics queue\n"); return 1; }
  float prio = 1;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci};
  VkDevice dev;
  CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
  VkQueue q;
  vkGetDeviceQueue(dev, fam, 0, &q);
  printf("ok device\n");

  // The image it draws into, and a buffer the CPU can read it from
  VkImageCreateInfo imci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                            {W, H, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
  VkImage img;
  CHECK(vkCreateImage(dev, &imci, NULL, &img));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, img, &mr);
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                              memory_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = memory_type(pd, mr.memoryTypeBits, 0);
  VkDeviceMemory imem;
  CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
  CHECK(vkBindImageMemory(dev, img, imem, 0));
  VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer buf;
  CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  mai.allocationSize = mr.size;
  mai.memoryTypeIndex = memory_type(pd, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (mai.memoryTypeIndex == UINT32_MAX) { printf("FAIL no memory the CPU can read\n"); return 1; }
  VkDeviceMemory bmem;
  CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
  CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
  VkImageViewCreateInfo ivci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D,
                                VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  VkImageView view;
  CHECK(vkCreateImageView(dev, &ivci, NULL, &view));

  // Render pass, framebuffer and the pipeline with the two shaders
  VkAttachmentDescription att = {0, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                 VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
  VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, NULL, 1, &ref};
  VkSubpassDependency dep = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0};
  VkRenderPassCreateInfo rpci = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, 1, &att, 1, &sub, 1, &dep};
  VkRenderPass rp;
  CHECK(vkCreateRenderPass(dev, &rpci, NULL, &rp));
  VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rp, 1, &view, W, H, 1};
  VkFramebuffer fb;
  CHECK(vkCreateFramebuffer(dev, &fci, NULL, &fb));
  VkShaderModuleCreateInfo smv = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof triangulo_vert, triangulo_vert};
  VkShaderModuleCreateInfo smf = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof triangulo_frag, triangulo_frag};
  VkShaderModule vs, fs;
  CHECK(vkCreateShaderModule(dev, &smv, NULL, &vs));
  CHECK(vkCreateShaderModule(dev, &smf, NULL, &fs));
  VkPipelineShaderStageCreateInfo st[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"}};
  VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
                                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport vp = {0, 0, W, H, 0, 1};
  VkRect2D sc = {{0, 0}, {W, H}};
  VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, &vp, 1, &sc};
  VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1;
  VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba = {0};
  cba.colorWriteMask = 0xf;
  VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1; cb.pAttachments = &cba;
  VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  VkPipelineLayout pl;
  CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
  VkGraphicsPipelineCreateInfo gpci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gpci.stageCount = 2; gpci.pStages = st; gpci.pVertexInputState = &vi; gpci.pInputAssemblyState = &ia;
  gpci.pViewportState = &vps; gpci.pRasterizationState = &rs; gpci.pMultisampleState = &ms; gpci.pColorBlendState = &cb;
  gpci.layout = pl; gpci.renderPass = rp;
  VkPipeline pipe;
  CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
  printf("ok pipeline\n");

  // Draw, then copy the image to the buffer
  VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam};
  VkCommandPool pool;
  CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
  VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cmd;
  CHECK(vkAllocateCommandBuffers(dev, &cbai, &cmd));
  VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  CHECK(vkBeginCommandBuffer(cmd, &cbbi));
  VkClearValue clear = {{{0.0f, 0.0f, 1.0f, 1.0f}}};   // blue
  VkRenderPassBeginInfo rpbi = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, rp, fb, {{0, 0}, {W, H}}, 1, &clear};
  vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  VkBufferImageCopy copy = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {W, H, 1}};
  vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &copy);
  VkBufferMemoryBarrier bb = {VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                              VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, buf, 0, VK_WHOLE_SIZE};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &bb, 0, NULL);
  CHECK(vkEndCommandBuffer(cmd));
  VkFenceCreateInfo fnci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence;
  CHECK(vkCreateFence(dev, &fnci, NULL, &fence));
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd};
  CHECK(vkQueueSubmit(q, 1, &si, fence));
  CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));
  printf("ok draw\n");

  unsigned char *px;
  CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));
  FILE *f = fopen("triangulo.ppm", "wb");
  if (f) {
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) fwrite(px + i * 4, 1, 3, f);
    fclose(f);
  }
  // A corner: the clear color (blue). The middle: inside the triangle, its fragment shader paints it orange
  const unsigned char *c = px + (2 * W + 2) * 4, *m = px + ((H / 2) * W + W / 2) * 4;
  printf("corner %d %d %d · middle %d %d %d\n", c[0], c[1], c[2], m[0], m[1], m[2]);
  int bad = 0;
  if (!(c[0] < 10 && c[1] < 10 && c[2] > 245)) { printf("FAIL corner isn't blue\n"); bad = 1; }
  else printf("ok clear\n");
  if (!(m[0] > 245 && m[1] > 120 && m[1] < 136 && m[2] < 10)) { printf("FAIL middle isn't orange (the triangle wasn't drawn)\n"); bad = 1; }
  else printf("ok triangle\n");
  vkUnmapMemory(dev, bmem);
  vkDeviceWaitIdle(dev);
  printf(bad ? "TRIANGULO: FALLÓ\n" : "TRIANGULO: TODO BIEN\n");
  return bad;
}
