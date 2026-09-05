/*
 * NetDialog Test - OS-driven infrastructure connect on a real PSP.
 *
 * Uses the sceUtilityNetconf dialog with PSP_NETCONF_ACTION_CONNECTAP to let
 * the PSP's own network stack establish the WLAN+DHCP link (the same path the
 * XMB "Test Connection" uses, and the method real games like Wipeout use).
 *
 * HEAVILY LOGGED: every step writes to ms0:/netdialog.log (open/write/close
 * per line) so a blank-screen hang still leaves an exact trail of where the
 * PSP stopped. Also logs the WLAN radio state on boot.
 *
 * After a successful dialog, grabs the IP and runs a TCP echo server on port
 * 23:  nc -v <PSP_IP> 23
 */
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspsdk.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <psputility_netmodules.h>
#include <pspwlan.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <stdio.h>

#define printf pspDebugScreenPrintf

#define MODULE_NAME "NetDialogTest"

PSP_MODULE_INFO(MODULE_NAME, 0, 1, 1);
PSP_HEAP_THRESHOLD_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-2048);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);

/* ---- log every step to ms0:/netdialog.log ---- */
static void log_line(const char *fmt, ...) {
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	SceUID fd = sceIoOpen("ms0:/netdialog.log",
	                      PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (fd >= 0) {
		sceIoWrite(fd, buf, (SceSize)strlen(buf));
		sceIoClose(fd);
	}
}

/* Exit callback */
int exit_callback(int arg1, int arg2, void *common)
{
	sceKernelExitGame();
	return 0;
}

/* Callback thread */
int CallbackThread(SceSize args, void *argp)
{
	int cbid;
	cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
	sceKernelRegisterExitCallback(cbid);
	sceKernelSleepThreadCB();
	return 0;
}

int SetupCallbacks(void)
{
	int thid = 0;
	thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0xFA0,
	                             PSP_THREAD_ATTR_USER, 0);
	if (thid >= 0) {
		int ret = sceKernelStartThread(thid, 0, 0);
		log_line("[cb] create=%d start=0x%X\n", thid, ret);
	} else {
		log_line("[cb] create failed: 0x%X\n", thid);
	}
	return thid;
}

/* ---- dialog driver (verbatim from the official PSPSDK netdialog sample) ---- */

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define PIXEL_SIZE (4)
#define FRAME_SIZE (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

static unsigned int __attribute__((aligned(16))) list[262144];

static void setupGu(void)
{
	log_line("[gu] init...\n");
	sceGuInit();
	sceGuStart(GU_DIRECT, list);
	sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
	sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)FRAME_SIZE, BUF_WIDTH);
	sceGuDepthBuffer((void *)(FRAME_SIZE * 2), BUF_WIDTH);
	sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
	sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
	sceGuDepthRange(0xc350, 0x2710);
	sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuFrontFace(GU_CW);
	sceGuShadeModel(GU_SMOOTH);
	sceGuEnable(GU_CULL_FACE);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuFinish();
	int sync = sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	log_line("[gu] done (sync=0x%X)\n", sync);
}

static void drawFrame(void)
{
	sceGuStart(GU_DIRECT, list);
	sceGuClearColor(0xff554433);
	sceGuClearDepth(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

void netInit(void)
{
	int r = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
	log_line("[net] sceNetInit=0x%X\n", r);
	r = sceNetInetInit();
	log_line("[net] sceNetInetInit=0x%X\n", r);
	r = sceNetApctlInit(0x8000, 48);
	log_line("[net] sceNetApctlInit=0x%X\n", r);
}

/* Runs the netconf dialog in CONNECTAP mode. Returns 1 if the OS reported a
 * successful connect (FINISHED), 0 otherwise. Blocks until done. */
int netDialog(void)
{
	pspUtilityNetconfData data;
	struct pspUtilityNetconfAdhoc adhocparam;

	memset(&data, 0, sizeof(data));
	data.base.size = sizeof(data);
	data.base.language = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	data.base.buttonSwap = PSP_UTILITY_ACCEPT_CROSS;
	data.base.graphicsThread = 17;
	data.base.accessThread = 19;
	data.base.fontThread = 18;
	data.base.soundThread = 16;
	data.action = PSP_NETCONF_ACTION_CONNECTAP;

	memset(&adhocparam, 0, sizeof(adhocparam));
	data.adhocparam = &adhocparam;

	int startret = sceUtilityNetconfInitStart(&data);
	log_line("[dlg] initstart=0x%X\n", startret);
	if (startret < 0)
		return 0;

	int done = 0;
	int status = PSP_UTILITY_DIALOG_NONE;
	int laststatus = -999;
	int frames = 0;
	while (1) {
		drawFrame();
		frames++;

		status = sceUtilityNetconfGetStatus();
		if (status != laststatus) {
			log_line("[dlg] status=%d (frame %d)\n", status, frames);
			laststatus = status;
		}
		if (status == PSP_UTILITY_DIALOG_VISIBLE) {
			int ur = sceUtilityNetconfUpdate(1);
			if (ur != 0)
				log_line("[dlg] update=%d at frame %d\n", ur, frames);
		} else if (status == PSP_UTILITY_DIALOG_QUIT) {
			int sr = sceUtilityNetconfShutdownStart();
			log_line("[dlg] shutdownstart=%d\n", sr);
		} else if (status == PSP_UTILITY_DIALOG_FINISHED) {
			log_line("[dlg] FINISHED at frame %d\n", frames);
			done = 1;
			break;
		} else if (status < 0) {
			log_line("[dlg] error status=%d at frame %d\n", status, frames);
			break;
		}

		if (frames % 120 == 0)
			log_line("[dlg] heartbeat frame %d status=%d\n", frames, status);

		sceDisplayWaitVblankStart();
		sceGuSwapBuffers();
	}

	log_line("[dlg] exited loop, done=%d status=%d\n", done, status);
	return done && (status == PSP_UTILITY_DIALOG_FINISHED);
}

/* ---- TCP echo server (same as nettest) ---- */

#define SERVER_PORT 23

int make_socket(uint16_t port)
{
	int sock;
	int ret;
	struct sockaddr_in name;

	sock = socket(PF_INET, SOCK_STREAM, 0);
	log_line("[srv] socket=%d errno=%d\n", sock, errno);
	if (sock < 0)
		return -1;

	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	ret = bind(sock, (struct sockaddr *)&name, sizeof(name));
	log_line("[srv] bind=%d errno=%d\n", ret, errno);
	if (ret < 0)
		return -1;

	return sock;
}

void start_server(const char *szIpAddr)
{
	int ret, sock, new = -1, readbytes;
	struct sockaddr_in client;
	socklen_t size = sizeof(client);
	char data[1024];
	fd_set set, setsave;

	sock = make_socket(SERVER_PORT);
	if (sock < 0) {
		log_line("[srv] make_socket failed\n");
		printf("Error creating server socket\n");
		return;
	}

	ret = listen(sock, 1);
	log_line("[srv] listen=%d errno=%d\n", ret, errno);
	if (ret < 0) {
		printf("Error calling listen\n");
		return;
	}

	printf("Listening for connections ip %s port %d\n", szIpAddr, SERVER_PORT);
	log_line("[srv] listening ip %s port %d\n", szIpAddr, SERVER_PORT);

	FD_ZERO(&set);
	FD_SET(sock, &set);
	setsave = set;

	while (1) {
		int i;
		set = setsave;
		if (select(FD_SETSIZE, &set, NULL, NULL, NULL) < 0) {
			printf("select error\n");
			return;
		}

		for (i = 0; i < FD_SETSIZE; i++) {
			if (FD_ISSET(i, &set)) {
				if (i == sock) {
					new = accept(sock, (struct sockaddr *)&client, &size);
					if (new < 0) {
						printf("Error in accept %s\n", strerror(errno));
						close(sock);
						return;
					}
					log_line("[srv] accepted from %s:%d fd=%d\n",
					         inet_ntoa(client.sin_addr), ntohs(client.sin_port), new);
					write(new, "Connected via OS dialog. Type away.\r\n", 37);
					FD_SET(new, &setsave);
				} else {
					readbytes = read(i, data, sizeof(data));
					if (readbytes <= 0) {
						printf("Socket closed\n");
						FD_CLR(i, &setsave);
						close(i);
					} else {
						write(i, data, readbytes);
						printf("%.*s", readbytes, data);
					}
				}
			}
		}
	}

	close(sock);
}

/* main routine */
int main(int argc, char *argv[])
{
	int sw, pwr;
	unsigned char mac[6];

	log_line("=== netdialog RUN ===\n");

	sw = sceWlanGetSwitchState();
	pwr = sceWlanDevIsPowerOn();
	log_line("[boot] wlan switch=%d power=%d\n", sw, pwr);
	if (sceWlanGetEtherAddr(mac) == 0)
		log_line("[boot] mac=%02X%02X%02X%02X%02X%02X\n",
		         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	else
		log_line("[boot] mac read failed\n");

	int m1 = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
	log_line("[boot] load COMMON=0x%X\n", m1);
	int m2 = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
	log_line("[boot] load INET=0x%X\n", m2);

	SetupCallbacks();
	setupGu();
	netInit();

	pspDebugScreenInit();

	if (netDialog())
	{
		union SceNetApctlInfo info;
		memset(&info, 0, sizeof(info));
		if (sceNetApctlGetInfo(8, &info) != 0)
			strcpy(info.ip, "unknown IP");
		log_line("[net] connected, info.ip=%s\n", info.ip);

		pspDebugScreenPrintf("OS dialog connected!\n");
		pspDebugScreenPrintf("IP: %s\n", info.ip);
		pspDebugScreenPrintf("\n");

		start_server(info.ip);
	}
	else
	{
		log_line("[net] dialog did NOT connect\n");
		pspDebugScreenPrintf("OS dialog did not connect\n");
		pspDebugScreenPrintf("Press HOME.\n");
	}

	sceKernelSleepThread();
	return 0;
}