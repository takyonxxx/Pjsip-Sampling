#include <stdio.h>
#include <stdlib.h>
#include <string.h>     // memset
#include <sys/types.h>  // open, lseek
#include <sys/stat.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>	// socket
#include <unistd.h>     // close, read
#include <time.h>       // time
#include <math.h>       /* round, floor, ceil, trunc */
#include <signal.h>		// signal
#include <pj/types.h>
#include <pjlib.h>
#include <pjmedia.h>
#include <pjmedia_audiodev.h>

#define THIS_FILE   "main.c"

#define SNDCARD_SAMPLING_RATE	96000	// 96000 192000
#define MILSEC_KBSP				16000

static volatile pj_bool_t g_run_app = PJ_TRUE;

//char *srvIP = "10.1.10.193";
char *srvIP = "127.0.0.1";
char log_buffer[250];

#define PORT 8888   //The port on which to send data
#define BUFF_SIZE 321
#define SAMPLE_SIZE 3840

char sendbuf[BUFF_SIZE];
struct sockaddr_in si_other;
int s,slen=sizeof(si_other);
pj_mutex_t *readMutex;
static unsigned countPositive = 0;
static unsigned countNegative = 0;
unsigned bufferCount = 0;

pjmedia_aud_param param;
pjmedia_aud_dev_index dev_idx = 0;

typedef enum { false, true } bool;

bool m_IsReading = false;

enum direction
{
    NEGATIVE = 0,
    POSITIVE = 1
};

static enum direction dir = POSITIVE;

void die(char *s)
{
    perror(s);
    exit(1);
}

int udp_start(char* srvip)
{
    if ( (s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
    {
        die("socket");
    }

    memset((char *) &si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(PORT);

    if (inet_aton(srvip , &si_other.sin_addr) == 0)
    {
        fprintf(stderr, "inet_aton() failed\n");
        return 0;
    }

    return s;
}

static void* checkSendBuffer( pj_int16_t *samples,pj_size_t size)
{    
    pj_int16_t value;
    unsigned i,bits;

    for (i = 0; i < size; ++i)
    {
        value = samples[i];

        if(value > 0)
        {
            if(dir == NEGATIVE)
            {
                if(countNegative > 0)
                {
                    dir = POSITIVE;

                    bits = (int) round(countNegative / 6.0);
                    int lenght = bufferCount + bits;

                    if(lenght > 320)
                    {
                        bits = 320 - bufferCount;
                    }

                    if(bits >= 1 &&  lenght <= BUFF_SIZE)
                    {
                        memset(&sendbuf[bufferCount], '0', bits);
                        bufferCount +=bits;
                    }
                    countNegative = 0;
                }         
            }
            
            countPositive++;
        }
        else
        {
            if(dir == POSITIVE)
            {
                if(countPositive > 0)
                {
                    dir = NEGATIVE;

                    bits = (int) round(countPositive / 6.0);

                    int lenght = bufferCount + bits;

                    if(lenght > 320)
                    {
                        bits = 320 - bufferCount;
                    }

                    if(bits >= 1 &&  lenght <= BUFF_SIZE)
                    {
                        memset(&sendbuf[bufferCount], '1', bits);
                        bufferCount +=bits;
                    }
                    countPositive = 0;
                }      
            }    
            
            countNegative++;
        }

       // printf("value: %d\n",value);
    }

    //printf("\nudp sendbuf(%d - %d):\n%s\n", bufferCount,size,sendbuf);

    sendbuf[bufferCount] = '\0';
    if (sendto(s, sendbuf, bufferCount , 0 , (struct sockaddr *) &si_other, slen)==-1)
    {
        printf("Udp send error...\n");
    }

    bufferCount = 0;

}

static void wait_thread(pj_thread_t **thread)
{
    while(1)
    {
        pj_thread_sleep(1 * 1000); // wait 10 msec
    }
}

static pj_status_t my_port_put_frame(struct pjmedia_port *this_port, pjmedia_frame *frame)
{ 
    int i;

    pj_mutex_lock(readMutex);

    pj_int16_t *samples = frame->buf;

    checkSendBuffer(samples,frame->size / 2);

    pj_mutex_unlock(readMutex);

    return PJ_SUCCESS;
}

static pj_status_t my_port_get_frame(struct pjmedia_port *this_port, pjmedia_frame *frame)
{
    PJ_LOG(3, (THIS_FILE, "media port: my_port_get_frame %lu", frame->size));

    frame->type = PJMEDIA_FRAME_TYPE_AUDIO;
    bzero(frame->buf, frame->size);

    return PJ_SUCCESS;
}

static pj_status_t my_port_on_destroy(struct pjmedia_port *this_port)
{
    PJ_LOG(3, (THIS_FILE, "media port: my_port_on_destroy"));
    return PJ_SUCCESS;
}

static void startSIP(const pjmedia_aud_param *param)
{
    pj_status_t status;
    pjmedia_endpt *med_endpt;

    pj_pool_factory *pf;

    pf = pjmedia_aud_subsys_get_pool_factory();

    status = pjmedia_endpt_create(pf, NULL, 1, &med_endpt);
    pj_assert(status == PJ_SUCCESS);

    pj_pool_t *pool;

    pool = pj_pool_create(pf, "pool1", 4000, 4000, NULL);
    pj_assert(pool != NULL);

    pj_mutex_create_simple(pool, "readMutex", &readMutex);

    // null port
    pjmedia_port *null_port;

    status = pjmedia_null_port_create(pool, param->clock_rate, param->channel_count, param->samples_per_frame, param->bits_per_sample, &null_port);
    pj_assert(status == PJ_SUCCESS);

    null_port->get_frame  = my_port_get_frame;
    null_port->put_frame  = my_port_put_frame;
    null_port->on_destroy = my_port_on_destroy;

    // sound device port
    pjmedia_snd_port *snd_port;

    status = pjmedia_snd_port_create_rec(pool, param->rec_id, param->clock_rate, param->channel_count, param->samples_per_frame, param->bits_per_sample, 0, &snd_port);
    pj_assert(status == PJ_SUCCESS);

    status = pjmedia_snd_port_connect(snd_port, null_port);
    pj_assert(status == PJ_SUCCESS);

    pj_thread_sleep(1 * 100); // wait 10 msec


    printf("\nclock_rate : %d\nchannel_count : %d\nsamples_per_frame : %d\nbits_per_sample : %d\n",
           param->clock_rate,param->channel_count,param->samples_per_frame,param->bits_per_sample );

    printf("\nSending UDP data to server %s  port %d\n",srvIP,PORT);
    printf("You may listen port on remote server by command: nc -u -l %d\n",PORT);

    pj_thread_t *thread = 0;
    wait_thread(&thread);

    status =  pjmedia_snd_port_destroy(snd_port);
    pj_assert(status == PJ_SUCCESS);

    status = pjmedia_port_destroy(null_port);
    pj_assert(status == PJ_SUCCESS);

    pj_pool_release(pool);

    status = pjmedia_endpt_destroy(med_endpt);
    pj_assert(status == PJ_SUCCESS);
}

void listAudioDevInfo()
{
    printf("\nSound Card List:\n\n");
    unsigned devCount = pjmedia_aud_dev_count();

    for (unsigned i = 0; i < devCount; i++)
    {
        pjmedia_aud_dev_info info;
        pj_status_t status = pjmedia_aud_dev_get_info(i, &info);

        if (status == PJ_SUCCESS)
        {
            sprintf(log_buffer, "Card Num: %2d - Card Name: %s %dHz",i,info.name,info.default_samples_per_sec);
            printf("%s\n",log_buffer);
        }
    }
    printf("\n");
}

static int main_func(int argc, char *argv[])
{   
    pj_status_t status;
    pj_caching_pool cp;

    char sndDevId[10];

    pj_log_set_level(0);

    status = pj_init();
    pj_assert(status == PJ_SUCCESS);

    PJ_LOG(3, (THIS_FILE, "Process ID: %d", pj_getpid()));

    pj_caching_pool_init(&cp, &pj_pool_factory_default_policy, 4 * 1024 * 1024);

    pjmedia_aud_subsys_init(&cp.factory);

    listAudioDevInfo();

    printf("Select Audio Device Number: (default = 0):\n");
    fflush(stdout);

    if (fgets(sndDevId, sizeof sndDevId, stdin) != NULL)
    {
        char *newline = strchr(sndDevId, '\n');
        if (newline)
            *newline = 0;

        if (strlen(sndDevId) != 0) // input was not empty
        {
            dev_idx = atoi(sndDevId);
        }
    }

    udp_start(srvIP);

    pjmedia_aud_dev_info info;
    status = pjmedia_aud_dev_get_info(dev_idx, &info);

    if (status == PJ_SUCCESS)
    {
        sprintf(log_buffer, "\nSelected Sound Device:\nCard Num: %2d\nCard Name: %s %dHz",dev_idx,info.name,info.default_samples_per_sec);
        printf("%s\n",log_buffer);
    }

    status = pjmedia_aud_dev_default_param(dev_idx, &param);
    pj_assert(status == PJ_SUCCESS);

    const int PTIME         = 20; // msec

    param.dir               = PJMEDIA_DIR_ENCODING;
    param.clock_rate        = SNDCARD_SAMPLING_RATE;
    param.channel_count     = 1;
    param.samples_per_frame = param.clock_rate * param.channel_count * PTIME / 1000;
    param.bits_per_sample   = 16;

    startSIP(&param);

    pjmedia_aud_subsys_shutdown();

    pj_caching_pool_destroy(&cp);

    PJ_LOG(3, (THIS_FILE, "Process terminated!"));

    pj_shutdown();

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[])
{
    return (pj_run_app(&main_func, argc, argv, 0));
}

