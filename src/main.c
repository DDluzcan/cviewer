#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

volatile sig_atomic_t terminal_resized = 0;

void handle_sigwinch(int sig) {
    terminal_resized = 1;
}

char* base64_encode(const uint8_t* data, size_t input_length, size_t* output_length) {
    *output_length = 4 * ((input_length + 2) / 3);
    char* encoded_data = malloc(*output_length + 1);
    if (!encoded_data) return NULL;

    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_b = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t octet_c = i < input_length ? (unsigned char)data[i++] : 0;
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;

        encoded_data[j++] = b64_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = b64_table[(triple >> 0 * 6) & 0x3F];
    }

    for (int i = 0; i < (3 - input_length % 3) % 3; i++) {
        encoded_data[*output_length - 1 - i] = '=';
    }
    encoded_data[*output_length] = '\0';
    return encoded_data;
}

void send_to_kitty_centered(uint8_t* pixels,int width,int height,int id,int start_col,int start_row,int term_cols,int term_rows) {
    size_t b64_len;
    char* b64_data = base64_encode(pixels, width * height * 4, &b64_len);
    if (!b64_data) return;

    printf("\033[2J\033[%d;%dH", start_row, start_col); 

    size_t offset = 0;
    int chunk_size = 4096;
    int is_first = 1;

    while (offset < b64_len) {
        size_t to_send = b64_len - offset;
        if (to_send > chunk_size) to_send = chunk_size;
        int has_more = (offset + to_send < b64_len) ? 1 : 0;

        if (is_first) {
            printf("\033_Ga=T,f=32,s=%d,v=%d,i=%d,m=%d;", width, height, id, has_more);
            is_first = 0;
        } else {
            printf("\033_Gm=%d;", has_more);
        }
        
        fwrite(b64_data + offset, 1, to_send, stdout);
        printf("\033\\");
        offset += to_send;
    }

    //mensaje de ayuda centrado en la parte inferior
    const char *help = "Move: Arrows Zoom: +/- or W/S Quit: Q";

    int help_col = (term_cols - (int)strlen(help)) / 2;
    if (help_col < 1)
        help_col = 1;

    printf("\033[%d;%dH%s", term_rows, help_col, help);

    fflush(stdout);
    free(b64_data);
}

uint8_t* crop_and_scale(uint8_t* src, int src_w, int src_h, int vp_w, int vp_h, float zoom, float pan_x, float pan_y, int* out_w, int* out_h) {
    float scaled_w = src_w * zoom;
    float scaled_h = src_h * zoom;

    *out_w = (scaled_w < vp_w) ? (int)scaled_w : vp_w;
    *out_h = (scaled_h < vp_h) ? (int)scaled_h : vp_h;

    if (*out_w < 1) *out_w = 1;
    if (*out_h < 1) *out_h = 1;

    uint8_t* dest = malloc((*out_w) * (*out_h) * 4);
    if (!dest) return NULL;

    float half_out_w = *out_w / 2.0f;
    float half_out_h = *out_h / 2.0f;

    for (int y = 0; y < *out_h; y++) {
        for (int x = 0; x < *out_w; x++) {
            int orig_x = (int)(pan_x + (x - half_out_w) / zoom);
            int orig_y = (int)(pan_y + (y - half_out_h) / zoom);

            if (orig_x < 0) orig_x = 0;
            if (orig_x >= src_w) orig_x = src_w - 1;
            if (orig_y < 0) orig_y = 0;
            if (orig_y >= src_h) orig_y = src_h - 1;

            int dest_idx = (y * (*out_w) + x) * 4;
            int src_idx = (orig_y * src_w + orig_x) * 4;

            dest[dest_idx] = src[src_idx];
            dest[dest_idx + 1] = src[src_idx + 1];
            dest[dest_idx + 2] = src[src_idx + 2];
            dest[dest_idx + 3] = src[src_idx + 3];
        }
    }
    return dest;
}

void clamp_pan(float* pan_x, float* pan_y, int src_w, int src_h, float zoom, int vp_w, int vp_h) {
    float scaled_w = src_w * zoom;
    float scaled_h = src_h * zoom;

    if (scaled_w <= vp_w) *pan_x = src_w / 2.0f; 
    else {
        float half_view_w = (vp_w / 2.0f) / zoom;
        if (*pan_x < half_view_w) *pan_x = half_view_w;
        if (*pan_x > src_w - half_view_w) *pan_x = src_w - half_view_w;
    }

    if (scaled_h <= vp_h) *pan_y = src_h / 2.0f; 
    else {
        float half_view_h = (vp_h / 2.0f) / zoom;
        if (*pan_y < half_view_h) *pan_y = half_view_h;
        if (*pan_y > src_h - half_view_h) *pan_y = src_h - half_view_h;
    }
}

void set_raw_mode(int enable) {
    static struct termios oldt, newt;
    if (enable) {
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);
        printf("\033[?25l"); 
    } else {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        printf("\033[?25h"); 
    }
    fflush(stdout);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("Uso: %s <imagen>\n", argv[0]);
        return 1;
    }

    int img_w, img_h, channels;
    uint8_t* img = stbi_load(argv[1], &img_w, &img_h, &channels, 4);
    if (!img) {
        printf("Error: No se pudo cargar '%s'\n", argv[1]);
        return 1;
    }

    signal(SIGWINCH, handle_sigwinch);

    int term_px_w, term_px_h, term_cols, term_rows, cell_w, cell_h;
    
    #define UPDATE_TERMINAL_SIZE() do { \
        struct winsize w; \
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w); \
        term_px_w = w.ws_xpixel ? w.ws_xpixel : 800; \
        term_px_h = w.ws_ypixel ? w.ws_ypixel : 600; \
        term_cols = w.ws_col ? w.ws_col : 80; \
        term_rows = w.ws_row ? w.ws_row : 24; \
        cell_w = term_px_w / term_cols; \
        cell_h = term_px_h / term_rows; \
    } while(0)

    UPDATE_TERMINAL_SIZE();

    float zoom = 1.0f;
    if (img_w > term_px_w || img_h > term_px_h) {
        float zoom_x = (float)term_px_w / img_w;
        float zoom_y = (float)term_px_h / img_h;
        zoom = (zoom_x < zoom_y) ? zoom_x : zoom_y;
    }

    float pan_x = img_w / 2.0f;
    float pan_y = img_h / 2.0f;

    set_raw_mode(1);
    int redraw = 1;
    int running = 1;

    struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };

    while (running) {
        if (terminal_resized) {
            UPDATE_TERMINAL_SIZE();
            terminal_resized = 0;
            redraw = 1;
        }

        if (redraw) {
            clamp_pan(&pan_x, &pan_y, img_w, img_h, zoom, term_px_w, term_px_h);
            
            int render_w, render_h;
            uint8_t* render_img = crop_and_scale(img, img_w, img_h, term_px_w, term_px_h, zoom, pan_x, pan_y, &render_w, &render_h);
            
            int start_col = (term_cols - (render_w / cell_w)) / 2;
            int start_row = (term_rows - (render_h / cell_h)) / 2;
            if (start_col < 1) start_col = 1;
            if (start_row < 1) start_row = 1;

            if (render_img) {
                send_to_kitty_centered(render_img,render_w,render_h,1,start_col,start_row,term_cols,term_rows);
                free(render_img);
            }
            redraw = 0;
        }

        int ret = poll(&pfd, 1, -1); 
        
        if (ret < 0 && errno == EINTR) {
            continue;
        }

        while (poll(&pfd, 1, 0) > 0) { 
            char c;
            if (read(STDIN_FILENO, &c, 1) <= 0) break;

            if (c == 'q' || c == 'Q') { running = 0; break; }

            else if (c == '+' || c == 'W' || c == 'w') { zoom += zoom * 0.2f; redraw = 1; }
            else if (c == '-' || c == 'S' || c == 's') { zoom -= zoom * 0.2f; if (zoom < 0.05f) zoom = 0.05f; redraw = 1; }

            /* I removed this controls cause i cant make it work, you can try to implement it if you want
            else if (c == 'j' || c == 'J') { pan_y += 20.0f / zoom; redraw = 1; }
            else if (c == 'l' || c == 'L') { pan_x += 20.0f / zoom; redraw = 1; }
            else if (c == 'h' || c == 'H') { pan_x -= 20.0f / zoom; redraw = 1; }
            */

            else if (c == '\033') {
                struct pollfd seq_pfd = { STDIN_FILENO, POLLIN, 0 };
                if (poll(&seq_pfd, 1, 15) > 0) { 
                    char seq[2];
                    read(STDIN_FILENO, &seq[0], 1);
                    if (poll(&seq_pfd, 1, 15) > 0) {
                        read(STDIN_FILENO, &seq[1], 1);
                        if (seq[0] == '[') {
                            float pan_speed = 60.0f / zoom; 
                            if (seq[1] == 'A') { pan_y -= pan_speed; redraw = 1; }      
                            else if (seq[1] == 'B') { pan_y += pan_speed; redraw = 1; } 
                            else if (seq[1] == 'C') { pan_x += pan_speed; redraw = 1; } 
                            else if (seq[1] == 'D') { pan_x -= pan_speed; redraw = 1; } 

                        }
                    }
                }
            }
        }
    }

    printf("\033_Ga=d,d=A;\033\\"); 
    printf("\033[2J\033[H");        
    set_raw_mode(0);
    stbi_image_free(img);

    return 0;
}