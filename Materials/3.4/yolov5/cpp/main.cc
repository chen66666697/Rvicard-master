#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>
#include <dirent.h>
#include <vector>
#include <string>
#include <algorithm>

#include "yolov5.h"
#include "image_utils.h"
#include "file_utils.h"
#include "image_drawing.h"

#if defined(RV1106_1103)
#include "dma_alloc.hpp"
#endif

#define MODEL_WIDTH 640   // 模型输入宽度
#define MODEL_HEIGHT 640  // 模型输入高度

// framebuffer 初始化
void* fb_init(int* fb, int* width, int* height, int* pixel_size) {
    *fb = open("/dev/fb0", O_RDWR);
    if(*fb < 0) return NULL;

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    ioctl(*fb, FBIOGET_VSCREENINFO, &vinfo);
    ioctl(*fb, FBIOGET_FSCREENINFO, &finfo);

    *width = vinfo.xres;
    *height = vinfo.yres;
    *pixel_size = vinfo.bits_per_pixel / 8;

    size_t screensize = (*width) * (*height) * (*pixel_size);
    void* framebuffer = mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, *fb, 0);
    if(framebuffer == MAP_FAILED) {
        close(*fb);
        return NULL;
    }

    return framebuffer;
}

// 获取文件夹下所有 jpg/png 文件
std::vector<std::string> get_frame_files(const std::string& folder) {
    std::vector<std::string> files;
    DIR* dir = opendir(folder.c_str());
    if(!dir) return files;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        std::string name = entry->d_name;
        if(name.size() > 4 &&
           (name.substr(name.size()-4) == ".jpg" || name.substr(name.size()-4) == ".png"))
            files.push_back(folder + "/" + name);
    }
    closedir(dir);
    std::sort(files.begin(), files.end());
    return files;
}

// RGB24 转 XRGB8888
void rgb24_to_xrgb8888(uint8_t* rgb, uint32_t* fb, int w, int h, int fb_width, int fb_height) {
    for(int y = 0; y < h && y < fb_height; y++) {
        for(int x = 0; x < w && x < fb_width; x++) {
            int idx = (y * w + x) * 3;
            uint8_t r = rgb[idx + 0];
            uint8_t g = rgb[idx + 1];
            uint8_t b = rgb[idx + 2];
            fb[y * fb_width + x] = (r << 16) | (g << 8) | b; // XRGB8888
        }
    }
}

int main(int argc, char **argv) {
    if(argc != 3) {
        printf("Usage: %s <model_path.rknn> <frames_folder>\n", argv[0]);
        return -1;
    }

    const char* model_path = argv[1];
    const char* frames_folder = argv[2];

    rknn_app_context_t rknn_app_ctx;
    memset(&rknn_app_ctx, 0, sizeof(rknn_app_context_t));

    init_post_process();
    if(init_yolov5_model(model_path, &rknn_app_ctx) != 0) {
        printf("init_yolov5_model fail!\n");
        return -1;
    }

    std::vector<std::string> frames = get_frame_files(frames_folder);
    if(frames.empty()) {
        printf("No frames found: %s\n", frames_folder);
        deinit_post_process();
        release_yolov5_model(&rknn_app_ctx);
        return -1;
    }

    int fb, fb_width, fb_height, fb_pixel_size;
    void* framebuffer = fb_init(&fb, &fb_width, &fb_height, &fb_pixel_size);
    if(!framebuffer) {
        printf("framebuffer init fail!\n");
        deinit_post_process();
        release_yolov5_model(&rknn_app_ctx);
        return -1;
    }

    size_t max_img_size = MODEL_WIDTH * MODEL_HEIGHT * 3;
    uint8_t* src_buf = (uint8_t*)malloc(max_img_size);
    if(!src_buf) {
        printf("malloc src_buf fail\n");
        return -1;
    }

    for(const auto& path : frames) {
        image_buffer_t src_image;
        memset(&src_image, 0, sizeof(image_buffer_t));
        src_image.virt_addr = src_buf;
        src_image.width  = MODEL_WIDTH;
        src_image.height = MODEL_HEIGHT;
        src_image.size   = MODEL_WIDTH * MODEL_HEIGHT * 3;

        if(read_image(path.c_str(), &src_image) != 0) {
            printf("read_image fail: %s\n", path.c_str());
            continue;
        }

#if defined(RV1106_1103)
        int ret = dma_buf_alloc(RV1106_CMA_HEAP_PATH, src_image.size,
                                &rknn_app_ctx.img_dma_buf.dma_buf_fd,
                                (void**)&rknn_app_ctx.img_dma_buf.dma_buf_virt_addr);
        memcpy(rknn_app_ctx.img_dma_buf.dma_buf_virt_addr, src_image.virt_addr, src_image.size);
        dma_sync_cpu_to_device(rknn_app_ctx.img_dma_buf.dma_buf_fd);
        src_image.virt_addr = (uint8_t*)rknn_app_ctx.img_dma_buf.dma_buf_virt_addr;
        src_image.fd = rknn_app_ctx.img_dma_buf.dma_buf_fd;
        rknn_app_ctx.img_dma_buf.size = src_image.size;
#endif

        object_detect_result_list od_results;
        if(inference_yolov5_model(&rknn_app_ctx, &src_image, &od_results) != 0) {
            printf("inference fail: %s\n", path.c_str());
#if !defined(RV1106_1103)
            free(src_image.virt_addr);
#endif
            continue;
        }

        char text[128];
        for(int i=0; i<od_results.count; i++) {
            object_detect_result *det = &od_results.results[i];
            draw_rectangle(&src_image, det->box.left, det->box.top,
                           det->box.right - det->box.left,
                           det->box.bottom - det->box.top,
                           COLOR_BLUE, 2);
            sprintf(text, "%s %.1f%%", coco_cls_to_name(det->cls_id), det->prop*100);
            draw_text(&src_image, text, det->box.left, det->box.top-20, COLOR_RED, 10);
        }

        // 显示到 framebuffer
        rgb24_to_xrgb8888(src_image.virt_addr, (uint32_t*)framebuffer,
                          src_image.width, src_image.height,
                          fb_width, fb_height);

#if defined(RV1106_1103)
        dma_buf_free(rknn_app_ctx.img_dma_buf.size,
                     &rknn_app_ctx.img_dma_buf.dma_buf_fd,
                     rknn_app_ctx.img_dma_buf.dma_buf_virt_addr);
#endif
    }

    free(src_buf);
    munmap(framebuffer, fb_width * fb_height * fb_pixel_size);
    close(fb);

    deinit_post_process();
    release_yolov5_model(&rknn_app_ctx);
    return 0;
}

