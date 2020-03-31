#include <stdio.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include "SDL2/SDL.h"
#include "SDL2/SDL_ttf.h"

#define _XOPEN_SOURCE
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/ioctl.h>

typedef int bool;
#define FALSE 0
#define TRUE 1

#include "vt100/vt100.h"

int fdm, fds;
int ssh_pid;
char pts_name[50];
#define SCREEN_WIDTH 880
#define SCREEN_HEIGHT 600

SDL_Renderer *renderer;
TTF_Font *font;
SDL_Texture *texture;
SDL_Texture *DrawString(TTF_Font *font, SDL_Renderer *renderer, SDL_Color fg, const char *s)
{
    SDL_Surface *surf = TTF_RenderText_Blended_Wrapped(font, s, fg, 680);
    SDL_Texture *text = SDL_CreateTextureFromSurface(renderer, surf);
    // int w,h;
    // SDL_QueryTexture(text, NULL,NULL,&w,&h);
    // printf("%d %d\n", w, h);
    SDL_FreeSurface(surf);
    return text;
}

struct vt100 *term = &(struct vt100){};

static void send_response(char *str)
{
    printf("\n[send_response]:[%s]\n",str);
    write(fdm, str, strlen(str));
}

// static void put_pixel(struct render_t * render, uint16_t x, uint16_t y, uint32_t color)
// {
// 	uint32_t * data = render->pixels;
// 	data[y * render->width + x] = color;
// }

// static uint32_t get_pixel(struct render_t * render, uint16_t x, uint16_t y)
// {
// 	uint32_t * data = render->pixels;
// 	return data[y * render->width + x];
// }

static void sync(struct vt100 *term)
{
}

static void _draw_char(struct vt100 *term, uint16_t x, uint16_t y, uint8_t ch, bool bsync)
{
    SDL_Color bg = *(SDL_Color *)&term->back_color;
    SDL_Color fg = *(SDL_Color *)&term->front_color;

    x = x * term->char_width;
    y = y * term->char_height;

    SDL_Texture *text = DrawString(font, renderer, fg, (char[]){ch, '\0'});
    SDL_SetRenderDrawColor(renderer, bg.r, bg.g, bg.b, bg.a);
    SDL_RenderFillRect(renderer, &(SDL_Rect){x, y, term->char_width, term->char_height});
    SDL_RenderCopy(renderer, text, NULL, &(SDL_Rect){x, y, term->font_width, term->font_height});
    SDL_DestroyTexture(text);
    if (bsync == TRUE)
    {
        //framebuffer_present_render(fb, render, &(struct dirty_rect_t) {x, y, term->char_width, term->char_height}, 1);
        //sync(term);
    }
}

static void draw_char(struct vt100 *term, uint16_t x, uint16_t y, uint8_t ch)
{
    _draw_char(term, x, y, ch, TRUE);
}

static void fill_rect(struct vt100 *term, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    SDL_SetRenderDrawColor(renderer, 0x00, 0x00, 0x00, 0x00);
    SDL_RenderFillRect(renderer, &(SDL_Rect){term->char_width * x, term->char_height * y, term->char_width * w, term->char_height * h});
    sync(term);
}

static void scroll(struct vt100 *term, int lines)
{
    uint16_t top = term->scroll_start_row;
    uint16_t bottom = term->scroll_end_row;
    int height = bottom - top + 1;
    int copy_lines, w, h, src_y, dis_y;
    if (lines > 0)
    {
        copy_lines = height - lines;
        dis_y = top * term->char_height;
        src_y = (top + lines) * term->char_height;
    }
    else if (lines < 0)
    {
        copy_lines = height + lines;
        dis_y = (top - lines) * term->char_height;
        src_y = top * term->char_height;
    }
    else
    {
        return;
    }

    w = term->screen_width;
    h = term->char_height * copy_lines;

    SDL_Color *pixelData = malloc(w * h * sizeof(SDL_Color));
    SDL_RenderReadPixels(renderer,
                         &(SDL_Rect){
                             0,
                             src_y,
                             w,
                             h},
                         SDL_PIXELFORMAT_RGBA8888,
                         pixelData, w * 4);
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(pixelData, w, h, 4 * 8, w * 4, SDL_PIXELFORMAT_RGBA8888);
    SDL_Texture *text = SDL_CreateTextureFromSurface(renderer, surf);
    SDL_FreeSurface(surf);
    term->fill_rect(term, 0, top, term->width, height);
    SDL_RenderCopy(renderer, text, NULL, &(SDL_Rect){0, dis_y, w, h});
    SDL_DestroyTexture(text);

    free(pixelData);
}

static void terminal_init(struct vt100 *term, int char_width, int char_height, int screen_width, int screen_height)
{
    term->char_width = char_width;
    term->char_height = char_height;
    term->font_width = term->char_width;
    term->font_height = term->char_height;
    term->screen_width = screen_width;
    term->screen_height = screen_height;
    term->width = term->screen_width / term->char_width;
    term->height = term->screen_height / term->char_height;
    term->draw_char = draw_char;
    term->fill_rect = fill_rect;
    term->scroll = scroll;
    term->send_response = send_response;
    vt100_init(term);
    //sync(term);
}

static ssize_t console_virt_read(struct vt100 *term, unsigned char *buf, size_t count)
{
    return count;
}

static ssize_t console_virt_write(struct vt100 *term, const unsigned char *buf, size_t count)
{
    vt100_puts(term, buf, count);
    //sync(pdat);
    return count;
}

int ptym_open(char *pts_name, int pts_namesz)
{
    int i;
    int p = open("/dev/ptmx", O_RDWR);
    if (p < 0)
    {
        printf("getpty(): couldn't get pty\n");
        close(p);
        return -1;
    }
    if (grantpt(p) < 0 || unlockpt(p) < 0)
    {
        printf("getpty(): couldn't grant and unlock pty\n");
        close(p);
        return -1;
    }

    if (isatty(p) && (ioctl(p, TIOCGPTN, &i) == 0))
    {
        sprintf(pts_name, "/dev/pts/%d", i);
        return p;
    }
    printf("getpty(): got pty %s\n", ptsname(p));
    if (ptsname(p) == NULL)
    {
        //perror("ptsname");
        //printf("errno = %d\n", errno);
    }
    strcpy(pts_name, (const char *)ptsname(p));

    return (p);
}

int ptys_open(char *pts_name)
{
    int fds;
    if ((fds = open(pts_name, O_RDWR)) < 0)
    {
        return -1;
    }
    return fds;
}
void exit_fn()
{
    kill(ssh_pid, SIGKILL);
    waitpid(ssh_pid, NULL, WUNTRACED);
}

void launch_shell()
{
    ssh_pid = fork();
    if (ssh_pid == 0)
    {
        int old=open("/dev/tty",O_RDWR);  //打开当前控制终端
	    ioctl(old, TIOCNOTTY);  //放弃当前控制终端
        if (setsid() < 0)
        {
            printf("setsid failed!!!!!");
            return -1;
        }

        if ((fds = ptys_open(pts_name)) < 0)
        {
            printf("ptys_open failed!!!!!");
            close(fdm);
            exit(0);
        }

        dup2(fds, STDIN_FILENO);
        dup2(fds, STDOUT_FILENO);
        dup2(fds, STDERR_FILENO);
        system("bash");
        while (1)
            ;
        exit(0);
    }
    else
    {
        //usleep(100);
        // if ((fds = ptys_open(pts_name)) < 0)
        // {
        //     printf("ptys_open failed!!!!!");
        //     close(fdm);
        //     exit(0);
        // }
        atexit(exit_fn);
    }

    return;
}

void launch_vt100()
{
    //屏幕宽度
    //初始化SDL
    setbuf(stdout,NULL);
    SDL_Init(SDL_INIT_EVERYTHING);
    //初始化字体库
    TTF_Init();
    //创建窗口
    SDL_Window *window = SDL_CreateWindow("VT100",
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_SHOWN);
    //创建渲染器
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);

    //打开字体
    font = TTF_OpenFont("Ubuntu Mono derivative Powerline.ttf", 20);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, SCREEN_WIDTH, SCREEN_HEIGHT);
    terminal_init(term, 10, 21, SCREEN_WIDTH, SCREEN_HEIGHT);
    console_virt_write(term, "\e[?7h", strlen("\e[?7h"));
    SDL_Event e;
    int quit = FALSE;

    //主循环
    while (!quit)
    {
        //事件栈轮询
        SDL_SetRenderTarget(renderer, texture);
        while (SDL_PollEvent(&e))
        {
            //按右上角的X或点鼠标退出
            switch (e.type)
            {
            case SDL_QUIT:
                exit(EXIT_SUCCESS);
                break;
            case SDL_KEYUP:
                if (e.key.keysym.sym == SDLK_RETURN)
                    write(fdm, (char[]){SDLK_RETURN}, 1);
                else if (e.key.keysym.sym == SDLK_BACKSPACE)
                    write(fdm, (char[]){SDLK_BACKSPACE}, 1);
                else if (e.key.keysym.sym == SDLK_TAB)
                    write(fdm, (char[]){SDLK_TAB}, 1);
                else if (e.key.keysym.sym == SDLK_UP)
                    write(fdm, "\e[A", 3);
                else
                    printf("[%d]\n",e.key.keysym.sym);
                break;
            case SDL_TEXTINPUT:
                write(fdm, e.text.text, strlen(e.text.text));
                break;
            }
        }

        {
            struct pollfd fdp;
            fdp.fd = fdm;
            fdp.events = POLLIN;
            int ret = poll(&fdp, 1, 10);
            if (ret < 0)
                return 1;
            if (fdp.revents & POLLERR)
                return 1;

            if (fdp.revents & POLLIN)
            {
                char c[200] = {0};
                if (read(fdm, c, 200 - 1) > 0)
                {
                    write(STDOUT_FILENO, c, strlen(c));
                    console_virt_write(term, c, strlen(c));
                }
            }
        }

        //呈现渲染器
        SDL_SetRenderTarget(renderer, NULL);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        int st;
        int pid = waitpid(ssh_pid, &st, WNOHANG);
        if (pid == ssh_pid && WIFEXITED(st))
        {
            break;
        }
    }

    //释放资源
    TTF_CloseFont(font);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);

    SDL_Quit();
}

int main(int argc, char **argv)
{
    if ((fdm = ptym_open(pts_name, sizeof(pts_name))) < 0)
    {
        printf("ptym_open error\r\n");
        return 0;
    }
    putenv("TERM=vt100");
    launch_shell();
    launch_vt100();
 
    return 0;
}
