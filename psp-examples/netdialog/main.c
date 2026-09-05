/*
 * NetDialog Test - OS-driven infrastructure connect on a real PSP.
 *
 * Uses the sceUtilityNetconf dialog with PSP_NETCONF_ACTION_CONNECTAP to let
 * the PSP's own network stack establish the WLAN+DHCP link (the same path the
 * XMB "Test Connection" uses, and the method real games like Wipeout use).
 * sceNetApctlConnect + manual DHCP is unreliable in homebrew (the DHCPREQUEST
 * often never leaves), which is exactly why nettest (apctl-direct) stalled.
 *
 * After the dialog reports FINISHED, we grab the IP and run the same TCP
 * echo server on port 23 so the PC can prove end-to-end sockets:
 *   nc -v <PSP_IP> 23
 *
 * Structure copied wholesale from the official PSPSDK netdialog sample.
 */
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspsdk.h>
#include <psputility.h>
#include <psputility_netconf.h>
#include <psputility_netmodules.h>
#include <pspkernel.h>
#include <psptypes.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

#define printf pspDebugScreenPrintf

#define MODULE_NAME "NetDialogTest"
#define HELLO_MSG "Connected via OS dialog. Type away.\r\n"

PSP_MODULE_INFO(MODULE_NAME, 0, 1, 1);
PSP_HEAP_THRESHOLD_SIZE_KB(1024);
PSP_HEAP_SIZE_KB(-2048);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);

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
	if (thid >= 0)
	{
		sceKernelStartThread(thid, 0, 0);
	}

	return thid;
}

/* ---- dialog driver (verbatim from the official sample) ---- */

#define BUF_WIDTH (512)
#define SCR_WIDTH (480)
#define SCR_HEIGHT (272)
#define PIXEL_SIZE (4)
#define FRAME_SIZE (BUF_WIDTH * SCR_HEIGHT * PIXEL_SIZE)

static unsigned int __attribute__((aligned(16))) list[262144];

static void setupGu(void)
{
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
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
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
	sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
	sceNetInetInit();
	sceNetApctlInit(0x8000, 48);
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
	if (startret < 0)
	{
		printf("sceUtilityNetconfInitStart: %08X\n", startret);
		return 0;
	}

	int done = 0;
	int status = PSP_UTILITY_DIALOG_NONE;
	while (1)
	{
		drawFrame();

		status = sceUtilityNetconfGetStatus();
		if (status == PSP_UTILITY_DIALOG_VISIBLE)
		{
			sceUtilityNetconfUpdate(1);
		}
		else if (status == PSP_UTILITY_DIALOG_QUIT)
		{
			sceUtilityNetconfShutdownStart();
		}
		else if (status == PSP_UTILITY_DIALOG_FINISHED)
		{
			done = 1;
			break;
		}
		else if (status < 0)
		{
			printf("Netconf dialog error: %08X\n", status);
			break;
		}

		sceDisplayWaitVblankStart();
		sceGuSwapBuffers();
	}

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
	if (sock < 0)
	{
		return -1;
	}

	name.sin_family = AF_INET;
	name.sin_port = htons(port);
	name.sin_addr.s_addr = htonl(INADDR_ANY);
	ret = bind(sock, (struct sockaddr *)&name, sizeof(name));
	if (ret < 0)
	{
		return -1;
	}

	return sock;
}

void start_server(const char *szIpAddr)
{
	int ret;
	int sock;
	int new = -1;
	struct sockaddr_in client;
	socklen_t size;
	int readbytes;
	char data[1024];
	fd_set set;
	fd_set setsave;

	sock = make_socket(SERVER_PORT);
	if (sock < 0)
	{
		printf("Error creating server socket\n");
		return;
	}

	ret = listen(sock, 1);
	if (ret < 0)
	{
		printf("Error calling listen\n");
		return;
	}

	printf("Listening for connections ip %s port %d\n", szIpAddr, SERVER_PORT);

	FD_ZERO(&set);
	FD_SET(sock, &set);
	setsave = set;

	while (1)
	{
		int i;
		set = setsave;
		if (select(FD_SETSIZE, &set, NULL, NULL, NULL) < 0)
		{
			printf("select error\n");
			return;
		}

		for (i = 0; i < FD_SETSIZE; i++)
		{
			if (FD_ISSET(i, &set))
			{
				if (i == sock)
				{
					new = accept(sock, (struct sockaddr *)&client, &size);
					if (new < 0)
					{
						printf("Error in accept %s\n", strerror(errno));
						close(sock);
						return;
					}

					printf("New connection from %s:%d\n", inet_ntoa(client.sin_addr),
								 ntohs(client.sin_port));

					write(new, HELLO_MSG, strlen(HELLO_MSG));

					FD_SET(new, &setsave);
				}
				else
				{
					readbytes = read(i, data, sizeof(data));
					if (readbytes <= 0)
					{
						printf("Socket closed\n");
						FD_CLR(i, &setsave);
						close(i);
					}
					else
					{
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
	sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
	sceUtilityLoadNetModule(PSP_NET_MODULE_INET);

	SetupCallbacks();
	setupGu();
	netInit();

	if (netDialog())
	{
		union SceNetApctlInfo info;
		memset(&info, 0, sizeof(info));
		if (sceNetApctlGetInfo(8, &info) != 0)
			strcpy(info.ip, "unknown IP");

		/* Debug text only AFTER the debug screen is initialised (a printf
		 * before pspDebugScreenInit() derefs a NULL buffer -> black screen). */
		pspDebugScreenInit();
		pspDebugScreenPrintf("OS dialog connected!\n");
		pspDebugScreenPrintf("IP: %s\n", info.ip);
		pspDebugScreenPrintf("\n");

		start_server(info.ip);
	}
	else
	{
		pspDebugScreenInit();
		pspDebugScreenPrintf("OS dialog did not connect\n");
		pspDebugScreenPrintf("Press HOME.\n");
	}

	sceKernelSleepThread();
	return 0;
}