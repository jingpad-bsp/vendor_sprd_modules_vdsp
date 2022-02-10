#include <utils/Mutex.h>
#include <utils/List.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <log/log_main.h>
#include "vdsp_dvfs.h"
#include "xrp_host_common.h"
#include "xrp_kernel_defs.h"
#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG  "vdsp_dvfs"

#define DVFS_MONITOR_CYCLE_TIME   (1000*1000)
using namespace android;
static Mutex timepiece_lock;
static Mutex powerhint_lock;
static uint32_t g_workingcount;
static pthread_t g_monitor_threadid;
static pthread_mutex_t g_deinitmutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_deinitcond = PTHREAD_COND_INITIALIZER;
#if 0
static struct vdsp_work_piece *g_currentpiece;
List<struct vdsp_work_piece*> g_workpieces_list;
#endif
static uint32_t g_monitor_exit;
static int64_t g_starttime; 
static int64_t g_cycle_totaltime;
static int64_t g_piece_starttime;
static uint32_t g_powerhint_count_level[SPRD_VDSP_POWERHINT_LEVEL_MAX];
static uint32_t g_last_powerhint_level = SPRD_VDSP_POWERHINT_RESTORE_DVFS;
static uint32_t g_last_dvfs_index = SPRD_VDSP_POWERHINT_RESTORE_DVFS;
static void *dvfs_monitor_thread(void* data);

int32_t set_dvfs_maxminfreq(void *device , int32_t maxminflag)
{
	struct xrp_dvfs_ctrl dvfs;
	struct xrp_device *dev = (struct xrp_device *)device;
	dvfs.en_ctl_flag = 0;
	dvfs.enable = 1;
	if(maxminflag)
		dvfs.index = SPRD_VDSP_POWERHINT_LEVEL_5;
	else
		dvfs.index = SPRD_VDSP_POWERHINT_LEVEL_0;
	ioctl(dev->impl.fd ,XRP_IOCTL_SET_DVFS , &dvfs);
	return 0;
}

int32_t init_dvfs(void* device)
{
	int ret , i;
	struct xrp_dvfs_ctrl dvfs;
	pthread_condattr_t attr;
	struct xrp_device *dev = (struct xrp_device *)device;
	g_monitor_exit = 0;
	g_last_powerhint_level = SPRD_VDSP_POWERHINT_RESTORE_DVFS;
	for(i = 0; i < SPRD_VDSP_POWERHINT_LEVEL_MAX; i++) {
		g_powerhint_count_level[i] = 0;
        }
	dvfs.en_ctl_flag = 1;
	dvfs.enable = 1;
	dvfs.index = SPRD_VDSP_POWERHINT_LEVEL_5;
	g_starttime = systemTime(CLOCK_MONOTONIC);
        g_cycle_totaltime = 0;
	g_last_dvfs_index = SPRD_VDSP_POWERHINT_RESTORE_DVFS;
	ioctl(dev->impl.fd ,XRP_IOCTL_SET_DVFS , &dvfs);
	pthread_condattr_init(&attr);
	pthread_mutex_init(&g_deinitmutex , NULL);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&g_deinitcond , &attr);
	pthread_condattr_destroy(&attr);
	ret = pthread_create(&g_monitor_threadid , NULL , dvfs_monitor_thread , device);
	if(0 == ret)
		return 1;/*1 is ok*/
	else {
		pthread_mutex_destroy(&g_deinitmutex);
		pthread_cond_destroy(&g_deinitcond);
		dvfs.en_ctl_flag = 0;
		return 0;/*0 is error*/
	}
}
void deinit_dvfs(void *device)
{
	struct xrp_device *dev = (struct xrp_device *)device;
	struct xrp_dvfs_ctrl dvfs;
	List<struct vdsp_work_piece*>::iterator iter;
	ALOGD("func:%s , enter" , __func__);
	/*remove all work pieces list*/
	pthread_mutex_lock(&g_deinitmutex);
	g_monitor_exit = 1;
	pthread_cond_signal(&g_deinitcond);
	pthread_mutex_unlock(&g_deinitmutex);
	pthread_join(g_monitor_threadid , NULL);
	dvfs.en_ctl_flag = 0;
	dvfs.enable = 1;
	pthread_mutex_destroy(&g_deinitmutex);
	pthread_cond_destroy(&g_deinitcond);
	ioctl(dev->impl.fd ,XRP_IOCTL_SET_DVFS , &dvfs);

#if 0
	for(iter = g_workpieces_list.begin(); iter != g_workpieces_list.end(); iter++) {
		delete *iter;
        }
	g_workpieces_list.clear();
#else
	ALOGD("func:%s , exit" , __func__);
#endif
}
static struct vdsp_work_piece* alloc_work_piece()
{
	struct vdsp_work_piece *piece = new struct vdsp_work_piece;//malloc(sizeof(struct vdsp_work_piece));
	if(piece) {
		piece->start_time = 0;
		piece->end_time = 0;
		piece->working_count = 0;
	}
	return piece;
}

void preprocess_work_piece()
{
#if 0
	struct vdsp_work_piece* piece;
#endif
	AutoMutex _l(timepiece_lock);
#if 0
	if(0 == g_workingcount) {
		piece = alloc_work_piece();
		g_currentpiece = piece;
		g_currentpiece->start_time = systemTime(CLOCK_MONOTONIC);
		g_currentpiece->working_count ++;
		g_workingcount ++;
		/*insert piece to piecelist*/
		g_workpieces_list.push_back(piece);
	}
	else {
		g_workingcount ++;
		g_currentpiece->working_count ++;
	}
#else
	if(0 == g_workingcount) {
		g_piece_starttime = systemTime(CLOCK_MONOTONIC);
	}
	g_workingcount ++;
#endif
}
void postprocess_work_piece()
{
	int64_t realstarttime;
	AutoMutex _l(timepiece_lock);
#if 0
	g_workingcount --;
	if(0 == g_workingcount) {
		g_currentpiece->end_time = systemTime(CLOCK_MONOTONIC);
	}
#else
	g_workingcount --;
	if(0 == g_workingcount) {
		realstarttime = g_piece_starttime > g_starttime ? g_piece_starttime : g_starttime;
		g_cycle_totaltime += (systemTime(CLOCK_MONOTONIC) - realstarttime);
	}
	ALOGD("func:%s , gworkingcount:%d" , __func__ , g_workingcount);
#endif
}
int32_t set_powerhint_flag(void *device , enum sprd_vdsp_power_level level , uint32_t acquire_release)
{
#if 0
	struct xrp_dvfs_ctrl dvfs;
	int i;
	int ret = 0;
	struct xrp_device *dev = (struct xrp_device *)device;
	if(level == SPRD_VDSP_POWERHINT_RESTORE_DVFS) {
		ALOGW("func:%s , level is error" , __func__);
		return -1;
	}
	powerhint_lock.lock();
	if(acquire_release == SPRD_VDSP_POWERHINT_ACQUIRE) {
		g_powerhint_count_level[level]++;
	} else if(SPRD_VDSP_POWERHINT_RELEASE == acquire_release){
		g_powerhint_count_level[level]--;
	}
	for(i = SPRD_VDSP_POWERHINT_LEVEL_MAX -1; i >= 0; i--) {
		if(g_powerhint_count_level[i] != 0) {
			break;
		}
	}

	ALOGD("%s acquire_release:%d , level:%d , i:%d , g_last_powerhint_level:%d\n" , __func__ , acquire_release , level,
		i , g_last_powerhint_level);
	if((i != -1) && (g_last_powerhint_level != i)) {
		/*set power hint*/
		dvfs.index = i;
		dvfs.en_ctl_flag = 0;
		ret = ioctl(dev->impl.fd , XRP_IOCTL_SET_DVFS , &dvfs);
		g_last_powerhint_level = i;
	} else if(-1 == i) {
		g_last_dvfs_index = g_last_powerhint_level;
		g_last_powerhint_level = SPRD_VDSP_POWERHINT_RESTORE_DVFS;
	}

	powerhint_lock.unlock();
	return ret;
#else
	int32_t ret;
	struct xrp_device *dev = (struct xrp_device*) device;
	struct xrp_powerhint_ctrl powerhint_info;
	powerhint_info.level = level;
	powerhint_info.acquire_release = acquire_release;
	ALOGD("fun:%s powerhint level:%d , acquire_release:%d" , __func__ , powerhint_info.level , powerhint_info.acquire_release);
	ret = ioctl(dev->impl.fd, XRP_IOCTL_SET_POWERHINT, &powerhint_info);
        return ret;
#endif
}
static uint32_t calculate_vdsp_usage(int64_t fromtime , __unused int64_t endtime)
{
#if 0
	int64_t costtime = 0;
	int64_t small_endtime;
	uint32_t count = 0;
	uint32_t removecount = 0;
	uint32_t notinrange = 0;
#if 0
	timepiece_lock.lock();
	timepiece_lock.unlock();
#endif
	/**/
	for(iter = g_workpieces_list.begin(); iter != g_workpieces_list.end(); iter++) {
		if(((*iter)->start_time <= fromtime) && ((*iter)->end_time) > fromtime) {
			costtime += ((*iter)->end_time - fromtime);
			count++;
		} else if(((*iter)->start_time >= fromtime) && ((*iter)->start_time < endtime)) {
			if((*iter)->end_time != 0) {
				small_endtime = (*iter)->end_time > endtime ? endtime : (*iter)->end_time;
				costtime += (small_endtime - (*iter)->start_time);
			}else {
				small_endtime = endtime;
				costtime += (endtime - (*iter)->start_time);
			}
			count++;
		} else {
			notinrange ++;
			/*time piece not between fromtime and endtime*/
			continue;
		}
	}
	/*lock here?*/
	timepiece_lock.lock();
	/*after this cycle total cost time is completed, remove the unused time pieces*/
	for(iter = g_workpieces_list.begin(); iter != g_workpieces_list.end();) {
		/*if time piece is older than fromtime to endtime erase and continue*/
		if(!(((*iter)->start_time >= endtime) || ((*iter)->end_time > endtime ) || (0 == (*iter)->end_time))) {
			delete *iter;
			iter = g_workpieces_list.erase(iter);
			removecount++;
			continue;
		}
		break;
	}
	timepiece_lock.unlock();
//	ALOGD(ANDROID_LOG_DEBUG,TAG_DVFS ,"%s count:%d, notinrange:%d , removecount:%d, costtime:%ld, end-from:%ld\n" ,
//				__func__ , count, notinrange , removecount , costtime , (endtime-fromtime));
	return (costtime*100) / (endtime - fromtime);
#else
	int32_t percent;
	int64_t current_time = systemTime(CLOCK_MONOTONIC);
	timepiece_lock.lock();
	if(g_workingcount != 0) {
		/*now some piece may executing*/
		if(g_piece_starttime <= fromtime) {
			g_cycle_totaltime = current_time - fromtime;
		} else {
			g_cycle_totaltime += (current_time - g_piece_starttime);
		}
	}
	percent = (g_cycle_totaltime*100) / (current_time - fromtime);
	ALOGD("func:%s , g_cycle_totaltime:%d ms , timeeclapse:%d ms , percent:%d" ,
			__func__ , (int)(g_cycle_totaltime/1000000) , (int)((current_time - fromtime)/1000000), percent);
	g_starttime = systemTime(CLOCK_MONOTONIC);
	g_cycle_totaltime = 0;
	timepiece_lock.unlock();
	return percent;
#endif
}
static uint32_t calculate_dvfs_index(uint32_t percent)
{
#if 1
	enum sprd_vdsp_power_level level = SPRD_VDSP_POWERHINT_LEVEL_0;
	static uint32_t last_percent = 0;
	if((last_percent > 50)) {
		if(percent > 50)
			level = SPRD_VDSP_POWERHINT_LEVEL_5;
		else if ((percent <= 50) && (percent > 20))
			level = SPRD_VDSP_POWERHINT_LEVEL_3;
		else if (percent <= 20)
			level = SPRD_VDSP_POWERHINT_LEVEL_2;
	} else if((last_percent <= 50) && (last_percent > 20)) {
		if(percent > 50)
			level = SPRD_VDSP_POWERHINT_LEVEL_5;
		else if((percent <= 50) && (percent > 20))
			level = SPRD_VDSP_POWERHINT_LEVEL_3;
		else if (percent <= 20)
			level = SPRD_VDSP_POWERHINT_LEVEL_2;
		else
			level = SPRD_VDSP_POWERHINT_LEVEL_2;
	} else if(last_percent <= 20){
		if(percent > 50)
			level = SPRD_VDSP_POWERHINT_LEVEL_5;
		else if ((percent <= 50) && (percent > 20))
			level = SPRD_VDSP_POWERHINT_LEVEL_2;
		else if (percent <= 20)
			level = SPRD_VDSP_POWERHINT_LEVEL_2;
	} else {
		level = SPRD_VDSP_POWERHINT_LEVEL_2;
	}
	last_percent = percent;
	return level;
#else
 	ALOGD("func:%s , force return level 5 , percent:%d" , __func__ , percent);
	return SPRD_VDSP_POWERHINT_LEVEL_5;
#endif
}
static int32_t calculate_delay_time(struct timespec *next_time , int32_t sleeptime)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}
	next_time->tv_sec = now.tv_sec + sleeptime;
	next_time->tv_nsec = now.tv_nsec;
	return 0;
}
static void *dvfs_monitor_thread(__unused void* data)
{
	uint32_t percentage;
	struct xrp_dvfs_ctrl dvfs;
	dvfs.en_ctl_flag = 0;
	struct timespec newtime;
	struct xrp_device *device = (struct xrp_device *)data;
	while(1) {
		pthread_mutex_lock(&g_deinitmutex);
		if(0 != g_monitor_exit) {
			pthread_mutex_unlock(&g_deinitmutex);
			ALOGD("%s exit\n" , __func__);
			break;
		}
		pthread_mutex_unlock(&g_deinitmutex);
		powerhint_lock.lock();
		if(SPRD_VDSP_POWERHINT_RESTORE_DVFS == g_last_powerhint_level) {
			percentage = calculate_vdsp_usage(g_starttime  , systemTime(CLOCK_MONOTONIC));
			ALOGD("%s percentage:%d\n" , __func__ , percentage);
			//g_starttime = systemTime(CLOCK_MONOTONIC);
			/*dvfs set freq*/
			dvfs.index = calculate_dvfs_index(percentage);
			if(dvfs.index != g_last_dvfs_index) {
				ALOGD("%s dvfs index:%d , last index:%d\n" , __func__ , dvfs.index , g_last_dvfs_index);
				ioctl(device->impl.fd , XRP_IOCTL_SET_DVFS , &dvfs);
				g_last_dvfs_index = dvfs.index;
			}
		}
		powerhint_lock.unlock();
		//usleep(DVFS_MONITOR_CYCLE_TIME);
		calculate_delay_time(&newtime , 1);
		pthread_mutex_lock(&g_deinitmutex);
		if(0 == g_monitor_exit)
			pthread_cond_timedwait_monotonic_np(&g_deinitcond,&g_deinitmutex, &newtime);
		pthread_mutex_unlock(&g_deinitmutex);
	}
	return NULL;
}
