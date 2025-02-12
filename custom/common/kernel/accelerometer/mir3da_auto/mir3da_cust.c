/* For MTK android platform.
 *
 * mir3da.c - Linux kernel modules for 3-Axis Accelerometer
 *
 * Copyright (C) 2011-2013 MiraMEMS Sensing Technology Co., Ltd.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>
#include <asm/atomic.h>

//#include <cust_acc_mir3da.h>
#include <cust_acc.h>
#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>
#include <linux/hwmsen_helper.h>
#include "mir3da_core.h"
#include "mir3da_cust.h"


#ifdef MT6516
#include <mach/mt6516_devs.h>
#include <mach/mt6516_typedefs.h>
#include <mach/mt6516_gpio.h>
#include <mach/mt6516_pll.h>
#endif

#ifdef MT6573
#include <mach/mt6573_devs.h>
#include <mach/mt6573_typedefs.h>
#include <mach/mt6573_gpio.h>
#include <mach/mt6573_pll.h>
#endif

#ifdef MT6575
#include <mach/mt6575_devs.h>
#include <mach/mt6575_typedefs.h>
#include <mach/mt6575_gpio.h>
#include <mach/mt6575_pm_ldo.h>
#endif


#ifdef MT6572
#include <mach/mt_pm_ldo.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_boot.h>
#endif

#ifdef MT6516
#define POWER_NONE_MACRO MT6516_POWER_NONE
#endif

#ifdef MT6573
#define POWER_NONE_MACRO MT65XX_POWER_NONE
#endif
#ifdef MT6575
#define POWER_NONE_MACRO MT65XX_POWER_NONE
#endif


#define POWER_NONE_MACRO MT65XX_POWER_NONE

#include <mach/mt_pm_ldo.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_boot.h>

#define MIR3DA_DRV_NAME                 	"mir3da"
#define MIR3DA_MISC_NAME                	"gsensor"
#define MIR3DA_PLATFORM_NAME         "gsensor"


#define MTK_ANDROID_23               	0

#define MIR3DA_AXIS_X          			0
#define MIR3DA_AXIS_Y         		 	1
#define MIR3DA_AXIS_Z          			2
#define MIR3DA_AXES_NUM        		3

#define MTK_AUTO_MODE           		1//0 

struct scale_factor{
    u8  whole;
    u8  fraction;
};

struct data_resolution {
    struct scale_factor scalefactor;
    int                 sensitivity;
};

struct mir3da_i2c_data {
    struct i2c_client 		*client;
    struct acc_hw 			*hw;
    struct hwmsen_convert   	cvt;
    
    struct data_resolution 	*reso;
    atomic_t                		trace;
    atomic_t                		suspend;
    atomic_t                		selftest;
    s16                     		cali_sw[MIR3DA_AXES_NUM+1];

    s8                      		offset[MIR3DA_AXES_NUM+1]; 
    s16                     		data[MIR3DA_AXES_NUM+1];

#if defined(CONFIG_HAS_EARLYSUSPEND)
    struct early_suspend    	early_drv;
#endif     
};

static struct data_resolution mir3da_data_resolution[] = {
    {{ 1, 0}, 1024},
};

static struct i2c_board_info      mir3da_i2c_boardinfo = { I2C_BOARD_INFO(MIR3DA_DRV_NAME, MIR3DA_I2C_ADDR>>1) };

static bool sensor_power = false;
static GSENSOR_VECTOR3D gsensor_gain, gsensor_offset;
static MIR_HANDLE              mir_handle;

extern int Log_level;
/*----------------------------------------------------------------------------*/
#define MI_DATA(format, ...)            if(DEBUG_DATA&Log_level){printk(KERN_ERR MI_TAG format "\n", ## __VA_ARGS__);}
#define MI_MSG(format, ...)             if(DEBUG_MSG&Log_level){printk(KERN_ERR MI_TAG format "\n", ## __VA_ARGS__);}
#define MI_ERR(format, ...)             if(DEBUG_ERR&Log_level){printk(KERN_ERR MI_TAG format "\n", ## __VA_ARGS__);}
#define MI_FUN                          if(DEBUG_FUNC&Log_level){printk(KERN_ERR MI_TAG "%s is called, line: %d\n", __FUNCTION__,__LINE__);}
#define MI_ASSERT(expr)                 \
	if (!(expr)) {\
		printk(KERN_ERR "Assertion failed! %s,%d,%s,%s\n",\
			__FILE__, __LINE__, __func__, #expr);\
	}
/*----------------------------------------------------------------------------*/
#if MTK_AUTO_MODE
static int mir3da_local_init(void);
static int mir3da_local_remove(void);
static struct sensor_init_info mir3da_init_info = {
    .name = MIR3DA_DRV_NAME,
    .init = mir3da_local_init,
    .uninit = mir3da_local_remove,
};
#else
static int mir3da_platform_probe(struct platform_device *pdev); 
static int mir3da_platform_remove(struct platform_device *pdev);
static struct platform_driver mir3da_gsensor_driver = {
    .driver     = {
        .name  = MIR3DA_PLATFORM_NAME,
        .owner = THIS_MODULE,
    },	
    .probe      = mir3da_platform_probe,
    .remove     = mir3da_platform_remove,    
};
#endif
/*----------------------------------------------------------------------------*/
#if MIR3DA_OFFSET_TEMP_SOLUTION
static char OffsetFileName[] = "/data/miraGSensorOffset.txt";
static int bCaliResult = -1; 
#define OFFSET_STRING_LEN               26
struct work_info
{
    char        tst1[20];
    char        tst2[20];
    char        buffer[OFFSET_STRING_LEN];
    struct      workqueue_struct *wq;
    struct      delayed_work read_work;
    struct      delayed_work write_work;
    struct      completion completion;
    int         len;
    int         rst; 
};

static struct work_info m_work_info = {{0}};
/*----------------------------------------------------------------------------*/
static void sensor_write_work( struct work_struct *work )
{
    struct work_info*   pWorkInfo;
    struct file         *filep;
    u32                 orgfs;
    int                 ret;   

    orgfs = get_fs();
    set_fs(KERNEL_DS);

    pWorkInfo = container_of((struct delayed_work*)work, struct work_info, write_work);
    if (pWorkInfo == NULL){            
            MI_ERR("get pWorkInfo failed!");       
            return;
    }
    
    filep = filp_open(OffsetFileName, O_RDWR|O_CREAT, 0600);
    if (IS_ERR(filep)){
        MI_ERR("write, sys_open %s error!!.\n", OffsetFileName);
        ret =  -1;
    }
    else
    {   
        filep->f_op->write(filep, pWorkInfo->buffer, pWorkInfo->len, &filep->f_pos);
        filp_close(filep, NULL);
        ret = 0;        
    }
    
    set_fs(orgfs);   
    pWorkInfo->rst = ret;
    complete( &pWorkInfo->completion );
}
/*----------------------------------------------------------------------------*/
static void sensor_read_work( struct work_struct *work )
{
    u32 orgfs;
    struct file *filep;
    int ret; 
    struct work_info* pWorkInfo;
        
    orgfs = get_fs();
    set_fs(KERNEL_DS);
    
    pWorkInfo = container_of((struct delayed_work*)work, struct work_info, read_work);
    if (pWorkInfo == NULL){            
        MI_ERR("get pWorkInfo failed!");       
        return;
    }
 
    filep = filp_open(OffsetFileName, O_RDONLY, 0600);
    if (IS_ERR(filep)){
        MI_ERR("read, sys_open %s error!!.\n",OffsetFileName);
        set_fs(orgfs);
        ret =  -1;
    }
    else{
        filep->f_op->read(filep, pWorkInfo->buffer,  sizeof(pWorkInfo->buffer), &filep->f_pos);
        filp_close(filep, NULL);    
        set_fs(orgfs);
        ret = 0;
    }

    pWorkInfo->rst = ret;
    complete( &(pWorkInfo->completion) );
}
/*----------------------------------------------------------------------------*/
static int sensor_sync_read(u8* offset)
{
    int     err;
    int     off[MIR3DA_OFFSET_LEN] = {0};
    struct work_info* pWorkInfo = &m_work_info;
     
    init_completion( &pWorkInfo->completion );
    queue_delayed_work( pWorkInfo->wq, &pWorkInfo->read_work, msecs_to_jiffies(0) );
    err = wait_for_completion_timeout( &pWorkInfo->completion, msecs_to_jiffies( 2000 ) );
    if ( err == 0 ){
        MI_ERR("wait_for_completion_timeout TIMEOUT");
        return -1;
    }

    if (pWorkInfo->rst != 0){
        MI_ERR("work_info.rst  not equal 0");
        return pWorkInfo->rst;
    }
    
    sscanf(m_work_info.buffer, "%x,%x,%x,%x,%x,%x,%x,%x,%x", &off[0], &off[1], &off[2], &off[3], &off[4], &off[5],&off[6], &off[7], &off[8]);

    offset[0] = (u8)off[0];
    offset[1] = (u8)off[1];
    offset[2] = (u8)off[2];
    offset[3] = (u8)off[3];
    offset[4] = (u8)off[4];
    offset[5] = (u8)off[5];
    offset[6] = (u8)off[6];
    offset[7] = (u8)off[7];
    offset[8] = (u8)off[8];
    
    return 0;
}
/*----------------------------------------------------------------------------*/
static int sensor_sync_write(u8* off)
{
    int err = 0;
    struct work_info* pWorkInfo = &m_work_info;
       
    init_completion( &pWorkInfo->completion );
    
    sprintf(m_work_info.buffer, "%x,%x,%x,%x,%x,%x,%x,%x,%x\n", off[0],off[1],off[2],off[3],off[4],off[5],off[6],off[7],off[8]);
    
    pWorkInfo->len = sizeof(m_work_info.buffer);
        
    queue_delayed_work( pWorkInfo->wq, &pWorkInfo->write_work, msecs_to_jiffies(0) );
    err = wait_for_completion_timeout( &pWorkInfo->completion, msecs_to_jiffies( 2000 ) );
    if ( err == 0 ){
        MI_ERR("wait_for_completion_timeout TIMEOUT");
        return -1;
    }

    if (pWorkInfo->rst != 0){
        MI_ERR("work_info.rst  not equal 0");
        return pWorkInfo->rst;
    }
    
    return 0;
}
/*----------------------------------------------------------------------------*/
static int check_califile_exist(void)
{
    u32     orgfs = 0;
    struct  file *filep;
        
    orgfs = get_fs();
    set_fs(KERNEL_DS);

    filep = filp_open(OffsetFileName, O_RDONLY, 0600);
    if (IS_ERR(filep)) {
        MI_ERR("%s read, sys_open %s error!!.\n",__func__,OffsetFileName);
        set_fs(orgfs);
        return -1;
    }

    filp_close(filep, NULL);    
    set_fs(orgfs); 

    return 0;
}
#endif
/*----------------------------------------------------------------------------*/
static int mir3da_resetCalibration(struct i2c_client *client)
{
    struct mir3da_i2c_data *obj = i2c_get_clientdata(client);    

    MI_FUN;
  
    memset(obj->cali_sw, 0x00, sizeof(obj->cali_sw));
    return 0;     
}
/*----------------------------------------------------------------------------*/
static int mir3da_readCalibration(struct i2c_client *client, int dat[MIR3DA_AXES_NUM])
{
    struct mir3da_i2c_data *obj = i2c_get_clientdata(client);

    MI_FUN;

    dat[obj->cvt.map[MIR3DA_AXIS_X]] = obj->cvt.sign[MIR3DA_AXIS_X]*obj->cali_sw[MIR3DA_AXIS_X];
    dat[obj->cvt.map[MIR3DA_AXIS_Y]] = obj->cvt.sign[MIR3DA_AXIS_Y]*obj->cali_sw[MIR3DA_AXIS_Y];
    dat[obj->cvt.map[MIR3DA_AXIS_Z]] = obj->cvt.sign[MIR3DA_AXIS_Z]*obj->cali_sw[MIR3DA_AXIS_Z];                        
                                       
    return 0;
}
/*----------------------------------------------------------------------------*/
static int mir3da_writeCalibration(struct i2c_client *client, int dat[MIR3DA_AXES_NUM])
{
    struct mir3da_i2c_data *obj = i2c_get_clientdata(client);
    int err = 0;
    int cali[MIR3DA_AXES_NUM];


    MI_FUN;
    if(!obj || ! dat)
    {
        MI_ERR("null ptr!!\n");
        return -EINVAL;
    }
    else
    {
        cali[obj->cvt.map[MIR3DA_AXIS_X]] = obj->cvt.sign[MIR3DA_AXIS_X]*obj->cali_sw[MIR3DA_AXIS_X];
        cali[obj->cvt.map[MIR3DA_AXIS_Y]] = obj->cvt.sign[MIR3DA_AXIS_Y]*obj->cali_sw[MIR3DA_AXIS_Y];
        cali[obj->cvt.map[MIR3DA_AXIS_Z]] = obj->cvt.sign[MIR3DA_AXIS_Z]*obj->cali_sw[MIR3DA_AXIS_Z]; 
        cali[MIR3DA_AXIS_X] += dat[MIR3DA_AXIS_X];
        cali[MIR3DA_AXIS_Y] += dat[MIR3DA_AXIS_Y];
        cali[MIR3DA_AXIS_Z] += dat[MIR3DA_AXIS_Z];

        obj->cali_sw[MIR3DA_AXIS_X] += obj->cvt.sign[MIR3DA_AXIS_X]*dat[obj->cvt.map[MIR3DA_AXIS_X]];
        obj->cali_sw[MIR3DA_AXIS_Y] += obj->cvt.sign[MIR3DA_AXIS_Y]*dat[obj->cvt.map[MIR3DA_AXIS_Y]];
        obj->cali_sw[MIR3DA_AXIS_Z] += obj->cvt.sign[MIR3DA_AXIS_Z]*dat[obj->cvt.map[MIR3DA_AXIS_Z]];
    } 

    return err;
}
/*----------------------------------------------------------------------------*/
static int mir3da_readChipInfo(struct i2c_client *client, char *buf, int bufsize)
{
    if((NULL == buf)||(bufsize<=30)) {
        return -1;
    }

    if(NULL == client) {
        *buf = 0;
        return -2;
    }

    sprintf(buf, "%s\n", "mir3da");
		
    return 0;
}
/*----------------------------------------------------------------------------*/
static int mir3da_setPowerMode(struct i2c_client *client, bool enable)
{
    int ret;
    
    MI_MSG ("mir3da_setPowerMode(), enable = %d", enable);
    ret = mir3da_set_enable(client,enable);  
    if (ret == 0){
        sensor_power = enable;
    }
    return ret;
}
/*----------------------------------------------------------------------------*/
static int mir3da_readSensorData_FMT(struct i2c_client *client, char *buf)
{    
    struct mir3da_i2c_data *obj = (struct mir3da_i2c_data*)i2c_get_clientdata(client);
    unsigned char databuf[20];
    int acc[MIR3DA_AXES_NUM];
    int res = 0;
    memset(databuf, 0, sizeof(unsigned char)*10);

    if(NULL == buf)
    {
        return -1;
    }
    if(NULL == client)
    {
        *buf = 0;
        return -2;
    }

    if(sensor_power == false)
    {
        res = mir3da_setPowerMode(client, true);
        if(res)
        {
            MI_ERR("Power on mir3da error %d!\n", res);
        }
        msleep(20);
    }

    if(res = mir3da_read_data(client, &(obj->data[MIR3DA_AXIS_X]),&(obj->data[MIR3DA_AXIS_Y]),&(obj->data[MIR3DA_AXIS_Z]))) 
    {        
        MI_ERR("I2C error: ret value=%d", res);
        return -3;
    }
    else
    {
        obj->data[MIR3DA_AXIS_X] += obj->cali_sw[MIR3DA_AXIS_X];
        obj->data[MIR3DA_AXIS_Y] += obj->cali_sw[MIR3DA_AXIS_Y];
        obj->data[MIR3DA_AXIS_Z] += obj->cali_sw[MIR3DA_AXIS_Z];
        
        acc[obj->cvt.map[MIR3DA_AXIS_X]] = obj->cvt.sign[MIR3DA_AXIS_X]*obj->data[MIR3DA_AXIS_X];
        acc[obj->cvt.map[MIR3DA_AXIS_Y]] = obj->cvt.sign[MIR3DA_AXIS_Y]*obj->data[MIR3DA_AXIS_Y];
        acc[obj->cvt.map[MIR3DA_AXIS_Z]] = obj->cvt.sign[MIR3DA_AXIS_Z]*obj->data[MIR3DA_AXIS_Z];

#if MIR3DA_STK_TEMP_SOLUTION
        if(bzstk)
          acc[MIR3DA_AXIS_Z] = abs(acc[MIR3DA_AXIS_Z]);
#endif
        MI_DATA("mir3da data map: %d, %d, %d!\n", acc[MIR3DA_AXIS_X], acc[MIR3DA_AXIS_Y], acc[MIR3DA_AXIS_Z]);
        
        if(abs((abs(acc[MIR3DA_AXIS_X])-1024)) < 150){
	     acc[MIR3DA_AXIS_X] =acc[MIR3DA_AXIS_X] +(((acc[MIR3DA_AXIS_X]-((acc[MIR3DA_AXIS_X]>0)?1:-1)*1024)>0)?-1:1)* abs(abs(acc[MIR3DA_AXIS_X])-1024)*3/4;
        }		 
        if(abs((abs(acc[MIR3DA_AXIS_Y])-1024)) < 150){
			 acc[MIR3DA_AXIS_Y] =acc[MIR3DA_AXIS_Y] +(((acc[MIR3DA_AXIS_Y]-((acc[MIR3DA_AXIS_Y]>0)?1:-1)*1024)>0)?-1:1)* abs(abs(acc[MIR3DA_AXIS_Y])-1024)*3/4;
        }
        if(abs(acc[MIR3DA_AXIS_Z]+1024) < 300){
			 acc[MIR3DA_AXIS_Z] =acc[MIR3DA_AXIS_Z] - abs(abs(acc[MIR3DA_AXIS_Z])-1024)*5/6;
        }		        
        
        acc[MIR3DA_AXIS_X] = acc[MIR3DA_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        acc[MIR3DA_AXIS_Y] = acc[MIR3DA_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        acc[MIR3DA_AXIS_Z] = acc[MIR3DA_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;        

        sprintf(buf, "%04x %04x %04x", acc[MIR3DA_AXIS_X], acc[MIR3DA_AXIS_Y], acc[MIR3DA_AXIS_Z]);
        
        MI_DATA( "mir3da data mg: x= %d, y=%d, z=%d\n",  acc[MIR3DA_AXIS_X],acc[MIR3DA_AXIS_Y],acc[MIR3DA_AXIS_Z]); 
    }
    
    return 0;
}
/*----------------------------------------------------------------------------*/
static int mir3da_readSensorData(struct i2c_client *client, char *buf)
{    
    struct mir3da_i2c_data *obj = (struct mir3da_i2c_data*)i2c_get_clientdata(client);
    unsigned char databuf[20];
    int acc[MIR3DA_AXES_NUM];
    int res = 0;
    memset(databuf, 0, sizeof(unsigned char)*10);

    if(NULL == buf)
    {
        return -1;
    }
    if(NULL == client)
    {
        *buf = 0;
        return -2;
    }

    if(sensor_power == false)
    {
        res = mir3da_setPowerMode(client, true);
        if(res)
        {
            MI_ERR("Power on mir3da error %d!\n", res);
        }
        msleep(20);
    }

    if(res = mir3da_read_data(client, &(obj->data[MIR3DA_AXIS_X]),&(obj->data[MIR3DA_AXIS_Y]),&(obj->data[MIR3DA_AXIS_Z]))) 
    {        
        MI_ERR("I2C error: ret value=%d", res);
        return -3;
    }
    else
    {
	#if MIR3DA_OFFSET_TEMP_SOLUTION
        if( 0 != bCaliResult ) 
	#endif    
        {
            obj->data[MIR3DA_AXIS_X] += obj->cali_sw[MIR3DA_AXIS_X];
            obj->data[MIR3DA_AXIS_Y] += obj->cali_sw[MIR3DA_AXIS_Y];
            obj->data[MIR3DA_AXIS_Z] += obj->cali_sw[MIR3DA_AXIS_Z];
        }
        
        acc[obj->cvt.map[MIR3DA_AXIS_X]] = obj->cvt.sign[MIR3DA_AXIS_X]*obj->data[MIR3DA_AXIS_X];
        acc[obj->cvt.map[MIR3DA_AXIS_Y]] = obj->cvt.sign[MIR3DA_AXIS_Y]*obj->data[MIR3DA_AXIS_Y];
        acc[obj->cvt.map[MIR3DA_AXIS_Z]] = obj->cvt.sign[MIR3DA_AXIS_Z]*obj->data[MIR3DA_AXIS_Z];

#if MIR3DA_STK_TEMP_SOLUTION
        if(bzstk)
          acc[MIR3DA_AXIS_Z] = abs(acc[MIR3DA_AXIS_Z]);
#endif

        MI_DATA("mir3da data map: %d, %d, %d!\n", acc[MIR3DA_AXIS_X], acc[MIR3DA_AXIS_Y], acc[MIR3DA_AXIS_Z]);
        
        acc[MIR3DA_AXIS_X] = acc[MIR3DA_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        acc[MIR3DA_AXIS_Y] = acc[MIR3DA_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        acc[MIR3DA_AXIS_Z] = acc[MIR3DA_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;        

        sprintf(buf, "%04x %04x %04x", acc[MIR3DA_AXIS_X], acc[MIR3DA_AXIS_Y], acc[MIR3DA_AXIS_Z]);
        
        MI_DATA( "mir3da data mg: x= %d, y=%d, z=%d\n",  acc[MIR3DA_AXIS_X],acc[MIR3DA_AXIS_Y],acc[MIR3DA_AXIS_Z]); 
    }
    
    return 0;
}
/*----------------------------------------------------------------------------*/
static int mir3da_readRawData(struct i2c_client *client, char *buf)
{
	struct mir3da_i2c_data *obj = (struct mir3da_i2c_data*)i2c_get_clientdata(client);
	int res = 0;

	if (!buf || !client) {
		return EINVAL;
	}
	
       res = mir3da_read_data(client, &(obj->data[MIR3DA_AXIS_X]),&(obj->data[MIR3DA_AXIS_Y]),&(obj->data[MIR3DA_AXIS_Z])); 
       if(res) {        
		MI_ERR("I2C error: ret value=%d", res);
		return EIO;
	}
	else {
		sprintf(buf, "%04x %04x %04x", obj->data[MIR3DA_AXIS_X], obj->data[MIR3DA_AXIS_Y], obj->data[MIR3DA_AXIS_Z]);
	
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static long mir3da_misc_ioctl( struct file *file,unsigned int cmd, unsigned long arg)
{
    struct i2c_client *client = mir_handle;
    struct mir3da_i2c_data *obj = (struct mir3da_i2c_data*)i2c_get_clientdata(client);    
    char strbuf[MIR3DA_BUFSIZE];
    void __user *data;
    SENSOR_DATA sensor_data;
    int err = 0;
    int cali[3]={0};

    if(_IOC_DIR(cmd) & _IOC_READ)
    {
        err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
    }
    else if(_IOC_DIR(cmd) & _IOC_WRITE)
    {
        err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
    }

    if(err)
    {
        MI_ERR("access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
        return -EFAULT;
    }
    
    switch (cmd) {  
 
    case GSENSOR_IOCTL_INIT:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_INIT\n");

	 err = mir3da_chip_resume(client);
	 if(err) {
		MI_ERR("chip resume fail!!\n");
		return;
	 }        
        break;

    case GSENSOR_IOCTL_READ_CHIPINFO:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_READ_CHIPINFO\n");
        data = (void __user *) arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }

	mir3da_readChipInfo(client,strbuf,MIR3DA_BUFSIZE);
		
        if(copy_to_user(data, strbuf, strlen(strbuf)+1))
        {
            err = -EFAULT;
            break;
        }                 
        break;      

    case GSENSOR_IOCTL_READ_SENSORDATA:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_READ_SENSORDATA\n");
        data = (void __user *) arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }
        
        mir3da_readSensorData_FMT(client, strbuf);
        if(copy_to_user(data, strbuf, strlen(strbuf)+1))
        {
            err = -EFAULT;
            break;      
        }                 
        break;

    case GSENSOR_IOCTL_READ_GAIN:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_READ_GAIN\n");
        data = (void __user *) arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }            
        
        if(copy_to_user(data, &gsensor_gain, sizeof(GSENSOR_VECTOR3D)))
        {
            err = -EFAULT;
            break;
        }                 
        break;

    case GSENSOR_IOCTL_READ_OFFSET:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_READ_OFFSET\n");
        data = (void __user *) arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }
        
        if(copy_to_user(data, &gsensor_offset, sizeof(GSENSOR_VECTOR3D)))
        {
            err = -EFAULT;
            break;
        }                 
        break;

    case GSENSOR_IOCTL_READ_RAW_DATA:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_READ_RAW_DATA\n");
        data = (void __user *) arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }
        mir3da_readRawData(client, &strbuf);
        if(copy_to_user(data, &strbuf, strlen(strbuf)+1))
        {
            err = -EFAULT;
            break;      
        }
        break;      

    case GSENSOR_IOCTL_SET_CALI:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_SET_CALI\n");
        data = (void __user*)arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }
        
        if(copy_from_user(&sensor_data, data, sizeof(sensor_data)))
        {
            err = -EFAULT;
            break;      
        }

        if(atomic_read(&obj->suspend))
        {
            MI_ERR("Perform calibration in suspend state!!\n");
            err = -EINVAL;
        }
        else
        {
            cali[MIR3DA_AXIS_X] = sensor_data.x * obj->reso->sensitivity / GRAVITY_EARTH_1000;
            cali[MIR3DA_AXIS_Y] = sensor_data.y * obj->reso->sensitivity / GRAVITY_EARTH_1000;
            cali[MIR3DA_AXIS_Z] = sensor_data.z * obj->reso->sensitivity / GRAVITY_EARTH_1000;              
            err = mir3da_writeCalibration(client, cali);
        }
        break;

    case GSENSOR_IOCTL_CLR_CALI:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_CLR_CALI\n");
        err = mir3da_resetCalibration(client);
        break;

    case GSENSOR_IOCTL_GET_CALI:
        MI_MSG("IOCTRL --- GSENSOR_IOCTL_GET_CALI\n");
        data = (void __user*)arg;
        if(data == NULL)
        {
            err = -EINVAL;
            break;      
        }
        if(err = mir3da_readCalibration(client, cali))
        {
            break;
        }
        
        sensor_data.x = cali[MIR3DA_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        sensor_data.y = cali[MIR3DA_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        sensor_data.z = cali[MIR3DA_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
        if(copy_to_user(data, &sensor_data, sizeof(sensor_data)))
        {
            err = -EFAULT;
            break;
        }        
        break;  
           
    default:
        return -EINVAL;
    }

    return err;
}
/*----------------------------------------------------------------------------*/
static const struct file_operations mir3da_misc_fops = {
        .owner = THIS_MODULE,
        .unlocked_ioctl = mir3da_misc_ioctl,
};

static struct miscdevice misc_mir3da = {
        .minor = MISC_DYNAMIC_MINOR,
        .name = MIR3DA_MISC_NAME,
        .fops = &mir3da_misc_fops,
};
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_enable_show(struct device_driver *ddri, char *buf)
{
    int ret;
    bool bEnable;

    MI_FUN;
	
    ret = mir3da_get_enable(mir_handle, &bEnable);   
    if (ret < 0){
        ret = -EINVAL;
    }
    else{
        ret = sprintf(buf, "%d\n", bEnable);
    }

    return ret;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_enable_store(struct device_driver *ddri, char *buf, size_t count)
{
    int ret;
    bool bEnable;
    unsigned long enable;

    if (buf == NULL){
        return -1;
    }

    enable = simple_strtoul(buf, NULL, 10);    
    bEnable = (enable > 0) ? true : false;

    ret = mir3da_set_enable (mir_handle, bEnable);
    if (ret < 0){
        ret = -EINVAL;
    }
    else{
        ret = count;
    }

    return ret;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_axis_data_show(struct device_driver *ddri, char *buf)
{
    int result;
    short x,y,z;
    int count = 0;

    result = mir3da_read_data(mir_handle, &x, &y, &z);
    if (result == 0)
        count += sprintf(buf+count, "x= %d;y=%d;z=%d\n", x,y,z);
    else
        count += sprintf(buf+count, "reading failed!");

    return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_reg_data_show(struct device_driver *ddri, char *buf)
{
    MIR_HANDLE          handle = mir_handle;
        
    return mir3da_get_reg_data(handle, buf);
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_reg_data_store(struct device_driver *ddri, char *buf, size_t count)
{
    int                 addr, data;
    int                 result;

    sscanf(buf, "0x%x, 0x%x\n", &addr, &data);
    
    result = mir3da_register_write(mir_handle, addr, data);
    
    MI_ASSERT(result==0);

    MI_MSG("set[0x%x]->[0x%x]\n",addr,data);	

    return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_log_level_show(struct device_driver *ddri, char *buf)
{
    int ret;

    ret = sprintf(buf, "%d\n", Log_level);

    return ret;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_log_level_store(struct device_driver *ddri, char *buf, size_t count)
{
    Log_level = simple_strtoul(buf, NULL, 10);
    return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_version_show(struct device_driver *ddri, char *buf)
{
	return sprintf(buf, "%s_%s\n", DRI_VER, CORE_VER);
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_vendor_show(struct device_driver *ddri, char *buf)
{
    return sprintf(buf, "%s\n", "MiraMEMS");
}
/*----------------------------------------------------------------------------*/
#if MIR3DA_OFFSET_TEMP_SOLUTION
static ssize_t mir3da_offset_show(struct device_driver *ddri, char *buf)
{
    ssize_t count = 0;
    int rst = 0;
    unsigned char off[9] = {0};
    
    rst = mir3da_read_offset(mir_handle,off);
    if (!rst){
        count = sprintf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n", off[0],off[1],off[2],off[3],off[4],off[5],off[6],off[7],off[8]);
    }
    return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_offset_store(struct device_driver *ddri, char *buf, size_t count)
{
    int off[9] = {0};
    u8  offset[9] = {0};
    int rst = 0;

    sscanf(buf, "%d,%d,%d,%d,%d,%d,%d,%d,%d\n", &off[0], &off[1], &off[2], &off[3], &off[4], &off[5],&off[6], &off[7], &off[8]);

    offset[0] = (unsigned char)off[0];
    offset[1] = (unsigned char)off[1];
    offset[2] = (unsigned char)off[2];
    offset[3] = (unsigned char)off[3];
    offset[4] = (unsigned char)off[4];
    offset[5] = (unsigned char)off[5];
    offset[6] = (unsigned char)off[6];
    offset[7] = (unsigned char)off[7];
    offset[8] = (unsigned char)off[8];

    rst = mir3da_write_offset(mir_handle,offset);
    return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_calibrate_show(struct device_driver *ddri, char *buf)
{
    int ret;       

    ret = sprintf(buf, "%d\n", bCaliResult);   
    return ret;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_calibrate_store(struct device_driver *ddri, char *buf, size_t count)
{
    signed char     z_dir = 0;
   
    z_dir = simple_strtol(buf, NULL, 10);
    bCaliResult = mir3da_calibrate(mir_handle,z_dir);
    
    return count;
}
#endif
/*----------------------------------------------------------------------------*/
#if FILTER_AVERAGE_ENHANCE
static ssize_t mir3da_average_enhance_show(struct device_driver *ddri, char *buf)
{
    int       ret = 0;
    struct mir3da_filter_param_s    param = {0};

    ret = mir3da_get_filter_param(&param);
    ret |= sprintf(buf, "%d %d %d\n", param.filter_param_l, param.filter_param_h, param.filter_threhold);

    return ret;
}
/*----------------------------------------------------------------------------*/
static ssize_t mir3da_average_enhance_store(struct device_driver *ddri, char *buf, size_t count)
{ 
    int       ret = 0;
    struct mir3da_filter_param_s    param = {0};
    
    sscanf(buf, "%d %d %d\n", &param.filter_param_l, &param.filter_param_h, &param.filter_threhold);
    
    ret = mir3da_set_filter_param(&param);
    
    return count;
}
#endif 
/*----------------------------------------------------------------------------*/
static DRIVER_ATTR(enable,          S_IRUGO | S_IWUGO,  mir3da_enable_show,             mir3da_enable_store);
static DRIVER_ATTR(axis_data,       S_IRUGO | S_IWUGO,  mir3da_axis_data_show,          NULL);
static DRIVER_ATTR(reg_data,        S_IWUGO | S_IRUGO,  mir3da_reg_data_show,           mir3da_reg_data_store);
static DRIVER_ATTR(log_level,       S_IWUGO | S_IRUGO,  mir3da_log_level_show,          mir3da_log_level_store);
static DRIVER_ATTR(vendor,          S_IWUGO | S_IRUGO,  mir3da_vendor_show,             NULL);
static DRIVER_ATTR(version,         S_IWUGO | S_IRUGO,  mir3da_version_show,            NULL); 
#if MIR3DA_OFFSET_TEMP_SOLUTION
static DRIVER_ATTR(offset,          S_IWUGO | S_IRUGO,  mir3da_offset_show,             mir3da_offset_store);
static DRIVER_ATTR(calibrate_miraGSensor,       S_IWUGO | S_IRUGO,  mir3da_calibrate_show,          mir3da_calibrate_store);
#endif
#if FILTER_AVERAGE_ENHANCE
static DRIVER_ATTR(average_enhance, S_IWUGO | S_IRUGO,  mir3da_average_enhance_show,    mir3da_average_enhance_store);
#endif 
/*----------------------------------------------------------------------------*/
static struct driver_attribute *mir3da_attributes[] = { 
    &driver_attr_enable,
    &driver_attr_axis_data,
    &driver_attr_reg_data,
    &driver_attr_log_level,
    &driver_attr_vendor,
    &driver_attr_version,
#if MIR3DA_OFFSET_TEMP_SOLUTION
    &driver_attr_offset,    
    &driver_attr_calibrate_miraGSensor,
#endif
#if FILTER_AVERAGE_ENHANCE
    &driver_attr_average_enhance,
#endif     
};
/*----------------------------------------------------------------------------*/
static int mir3da_create_attr(struct device_driver *driver) 
{
    int idx, err = 0;
    int num = (int)(sizeof(mir3da_attributes)/sizeof(mir3da_attributes[0]));
    if (driver == NULL)
    {
        return -EINVAL;
    }

    for(idx = 0; idx < num; idx++)
    {
        if(err = driver_create_file(driver, mir3da_attributes[idx]))
        {            
            MI_MSG("driver_create_file (%s) = %d\n", mir3da_attributes[idx]->attr.name, err);
            break;
        }
    }    
    return err;
}
/*----------------------------------------------------------------------------*/
static int mir3da_delete_attr(struct device_driver *driver)
{
    int idx ,err = 0;
    int num = (int)(sizeof(mir3da_attributes)/sizeof(mir3da_attributes[0]));

    if(driver == NULL)
    {
        return -EINVAL;
    }

    for(idx = 0; idx < num; idx++)
    {
        driver_remove_file(driver, mir3da_attributes[idx]);
    }

    return err;
}

/*----------------------------------------------------------------------------*/
static void mir3da_power(struct acc_hw *hw, unsigned int on) 
{
    static unsigned int power_on = 0;

    MI_MSG("power %s\n", on ? "on" : "off");


    if(hw->power_id != POWER_NONE_MACRO)       
    {        
        MI_MSG("power %s\n", on ? "on" : "off");
        if(power_on == on)    
        {
            MI_MSG("ignore power control: %d\n", on);
        }
        else if(on)   
        {
            if(!hwPowerOn(hw->power_id, hw->power_vol, "mir3da"))
            {
                MI_ERR("power on fails!!\n");
            }
        }
        else    
        {
            if (!hwPowerDown(hw->power_id, "mir3da"))
            {
                MI_ERR("power off fail!!\n");
            }              
        }
    }

    power_on = on;    
}

#ifndef CONFIG_HAS_EARLYSUSPEND
/*----------------------------------------------------------------------------*/
static int mir3da_suspend(struct i2c_client *client, pm_message_t msg) 
{
	struct mir3da_i2c_data *obj = i2c_get_clientdata(client);    
	int err = 0;
	u8 dat;
	MI_FUN;    

	if(msg.event == PM_EVENT_SUSPEND)
	{   
		if(obj == NULL)
		{
			MI_ERR("null pointer!!\n");
			return -EINVAL;
		}
		
		err = mir3da_set_enable(obj->client, false);
        if(err) {
            return res;
        }
                
		mir3da_power(obj->hw, 0);
	}
	return err;
}
/*----------------------------------------------------------------------------*/
static int mir3da_resume(struct i2c_client *client)
{
	struct mir3da_i2c_data *obj = i2c_get_clientdata(client);        
	int err;
	MI_FUN;

	if(obj == NULL)
	{
		MI_ERR("null pointer!!\n");
		return -EINVAL;
	}
	
       err = mir3da_chip_resume(obj->client);
	if(err) {
		MI_ERR("chip resume fail!!\n");
		return;
	}

        err = mir3da_setPowerMode(obj->client, true);
        if(err != 0) {
		return err;
        }		

	mir3da_power(obj->hw, 1);
    
	atomic_set(&obj->suspend, 0);

	return 0;
}
#else 
/*----------------------------------------------------------------------------*/
static void mir3da_early_suspend(struct early_suspend *h) 
{
	struct mir3da_i2c_data *obj = container_of(h, struct mir3da_i2c_data, early_drv);   
	int err;
	MI_FUN;    

	if(obj == NULL)
	{
		MI_ERR("null pointer!!\n");
		return;
	}
	atomic_set(&obj->suspend, 1); 

	err = mir3da_setPowerMode(obj->client, false);
	if(err) {
		MI_ERR("write power control fail!!\n");
		return;
	}

	sensor_power = false;
	
	mir3da_power(obj->hw, 0);
}

/*----------------------------------------------------------------------------*/
static void mir3da_late_resume(struct early_suspend *h)
{
	struct mir3da_i2c_data  *obj = container_of(h, struct mir3da_i2c_data, early_drv);         
	int                     err;

	MI_FUN;

	if(obj == NULL) {
		MI_ERR("null pointer!!\n");
		return;
	}

        err = mir3da_chip_resume(obj->client);
	if(err) {
		MI_ERR("chip resume fail!!\n");
		return;
	}


        err = mir3da_setPowerMode(obj->client, true);
        if(err != 0) {
		return err;
        }		

	mir3da_power(obj->hw, 1);

	atomic_set(&obj->suspend, 0);    
}
#endif 
/*----------------------------------------------------------------------------*/
static int mir3da_operate(void* self, uint32_t command, void* buff_in, int size_in,
        void* buff_out, int size_out, int* actualout)
{
    int err = 0;
    int value;    
    struct mir3da_i2c_data *priv = (struct mir3da_i2c_data*)self;
    hwm_sensor_data* gsensor_data;
    char buff[MIR3DA_BUFSIZE];
    
    switch (command)
    {
        case SENSOR_DELAY:
            MI_MSG("mir3da_operate, command is SENSOR_DELAY");
            break;

        case SENSOR_ENABLE:
            MI_MSG("mir3da_operate enable gsensor\n");
            if((buff_in == NULL) || (size_in < sizeof(int)))
            {
                MI_ERR("mir3da_operate Enable sensor parameter error!\n");
                err = -EINVAL;
            }
            else
            {
                value = *(int *)buff_in;
                
                MI_MSG("mir3da_operate, command is SENSOR_ENABLE, value = %d\n", value);
                
                if(((value == 0) && (sensor_power == false)) ||((value == 1) && (sensor_power == true)))
                {
                    MI_MSG("Gsensor device have updated!\n");
                }
                else
                {
                    err = mir3da_setPowerMode( priv->client, !sensor_power);
                }
            }
            break;

        case SENSOR_GET_DATA:
            MI_MSG("mir3da_operate, command is SENSOR_GET_DATA\n");
            if((buff_out == NULL) || (size_out< sizeof(hwm_sensor_data)))
            {
                MI_MSG("get sensor data parameter error!\n");
                err = -EINVAL;
            }
            else
            {
                gsensor_data = (hwm_sensor_data *)buff_out;
               
                mir3da_readSensorData(priv->client, buff);  
                sscanf(buff, "%x %x %x", &gsensor_data->values[0], 
                &gsensor_data->values[1], &gsensor_data->values[2]);
                
                gsensor_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;                
                gsensor_data->value_divide = 1000;                
            }
            break;
        default:
                MI_MSG("gsensor operate function no this parameter %d!\n", command);
                err = -1;
            break;
    }
    
    return err;
}
/*----------------------------------------------------------------------------*/
int i2c_smbus_read(PLAT_HANDLE handle, u8 addr, u8 *data)
{
    int                 res = 0;
    struct i2c_client   *client = (struct i2c_client*)handle;
    
    *data = i2c_smbus_read_byte_data(client, addr);
    
    return res;
}
/*----------------------------------------------------------------------------*/
int i2c_smbus_read_block(PLAT_HANDLE handle, u8 addr, u8 count, u8 *data)
{
    int                 res = 0;
    struct i2c_client   *client = (struct i2c_client*)handle;
    
    res = i2c_smbus_read_i2c_block_data(client, addr, count, data);
    
    return res;
}
/*----------------------------------------------------------------------------*/
int i2c_smbus_write(PLAT_HANDLE handle, u8 addr, u8 data)
{
    int                 res = 0;
    struct i2c_client   *client = (struct i2c_client*)handle;
    
    res = i2c_smbus_write_byte_data(client, addr, data);
    
    return res;
}
/*----------------------------------------------------------------------------*/
void msdelay(int ms)
{
    mdelay(ms);
}

#if MIR3DA_OFFSET_TEMP_SOLUTION
MIR_GENERAL_OPS_DECLARE(ops_handle, i2c_smbus_read, i2c_smbus_read_block, i2c_smbus_write, sensor_sync_write, sensor_sync_read, msdelay, printk, sprintf);
#else
MIR_GENERAL_OPS_DECLARE(ops_handle, i2c_smbus_read, i2c_smbus_read_block, i2c_smbus_write, NULL, NULL, msdelay, printk, sprintf);
#endif
/*----------------------------------------------------------------------------*/
static int __devinit mir3da_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int                 result;
    struct mir3da_i2c_data *obj;
    struct hwmsen_object sobj;    

    MI_FUN;
 
    
    if(!(obj = kzalloc(sizeof(*obj), GFP_KERNEL)))
    {
        MI_ERR("kzalloc failed!");
        result = -ENOMEM;
        goto exit;
    }   
    
    memset(obj, 0, sizeof(struct mir3da_i2c_data));
	
    obj->client = client;
    i2c_set_clientdata(client,obj);	

    obj->hw = mir3da_get_cust_acc_hw();	
    
    if(result = hwmsen_get_convert(obj->hw->direction, &obj->cvt))
    {
        MI_ERR("invalid direction: %d\n", obj->hw->direction);
        goto exit;
    }   
	
    obj->reso = &mir3da_data_resolution[0];
    gsensor_gain.x = gsensor_gain.y = gsensor_gain.z = obj->reso->sensitivity;

    result = mir3da_resetCalibration(client);
    if(result != 0) {
	   return result;
    }	

    atomic_set(&obj->trace, 0);
    atomic_set(&obj->suspend, 0);

    if(mir3da_install_general_ops(&ops_handle)){
        MI_ERR("Install ops failed !\n");
        goto exit_init_failed;
    }

#if MIR3DA_OFFSET_TEMP_SOLUTION
    m_work_info.wq = create_singlethread_workqueue( "oo" );
    if(NULL==m_work_info.wq) {
        MI_ERR("Failed to create workqueue !");
        goto exit_init_failed;
    }
    
    INIT_DELAYED_WORK( &m_work_info.read_work, sensor_read_work );
    INIT_DELAYED_WORK( &m_work_info.write_work, sensor_write_work );
    bCaliResult = check_califile_exist();
#endif
    
    mir_handle = mir3da_core_init(client);
    if(NULL == mir_handle){
        MI_ERR("chip init failed !\n");
        goto exit_init_failed;        
    }

    result = misc_register(&misc_mir3da);
    if (result) {
        MI_ERR("%s: misc register failed !\n", __func__);
        goto exit_misc_device_register_failed;
    }

#if MTK_AUTO_MODE    
    if(result = mir3da_create_attr(&(mir3da_init_info.platform_diver_addr->driver)))
    {
        MI_ERR("create attribute result = %d\n", result);
        result = -EINVAL;
	 goto exit_create_attr_failed;
    }
#else
    if(result = mir3da_create_attr(&mir3da_gsensor_driver.driver))
    {
        MI_ERR("create attribute result = %d\n", result);
        result = -EINVAL;		
        goto exit_create_attr_failed;
    }
#endif    
  
    sobj.self = obj;
    sobj.polling = 1;
    sobj.sensor_operate =mir3da_operate;
    if(result = hwmsen_attach(ID_ACCELEROMETER, &sobj))
    {
        MI_ERR("attach fail = %d\n", result);
        goto exit_kfree;
    }
    
#ifdef CONFIG_HAS_EARLYSUSPEND
	obj->early_drv.level    = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1,
	obj->early_drv.suspend  = mir3da_early_suspend,
	obj->early_drv.resume   = mir3da_late_resume,    
	register_early_suspend(&obj->early_drv);
#endif

    return result;
exit_create_attr_failed:
	misc_deregister(&misc_mir3da);
	exit_misc_device_register_failed:
	exit_init_failed:
	exit_kfree:
	kfree(obj);
	exit:
	MI_ERR("%s: err = %d\n", __func__, result);        
	return result;
}
/*----------------------------------------------------------------------------*/
static int  mir3da_remove(struct i2c_client *client)
{
   int err = 0;	

#if MTK_AUTO_MODE    
    if(err = mir3da_delete_attr(&(mir3da_init_info.platform_diver_addr->driver)))
    {
        MI_ERR("mir3da_delete_attr fail: %d\n", err);
    }
#else
    if(err = mir3da_delete_attr(&mir3da_gsensor_driver.driver))
    {
        MI_ERR("mir3da_delete_attr fail: %d\n", err);
    }
#endif 
    misc_deregister(&misc_mir3da);
    mir_handle = NULL;
    i2c_unregister_device(client);
    kfree(i2c_get_clientdata(client));    
    
    return 0;
}

/*----------------------------------------------------------------------------*/
static int mir3da_detect(struct i2c_client *new_client,int kind,struct i2c_board_info *info)
{
      struct i2c_adapter *adapter = new_client->adapter;

      MI_MSG("mir3da_detect, bus[%d] addr[0x%x]\n", adapter->nr,new_client->addr);

      if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

       strcpy(info->type, MIR3DA_DRV_NAME);

       return 0;
}
/*----------------------------------------------------------------------------*/
static const struct i2c_device_id mir3da_id[] = {
    { MIR3DA_DRV_NAME, 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, mir3da_id);

#if MTK_ANDROID_23
static unsigned short mir3da_force[] = {0x00, MIR3DA_I2C_ADDR, I2C_CLIENT_END, I2C_CLIENT_END};
static const unsigned short *const mir3da_forces[] = { mir3da_force, NULL };
static struct i2c_client_address_data mir3da_addr_data = { .forces = mir3da_forces,};
#endif

static struct i2c_driver mir3da_driver = {
    .driver = {
        .name    = MIR3DA_DRV_NAME,
        .owner   = THIS_MODULE,
    },    
    .probe       = mir3da_probe,
    .remove      = mir3da_remove,
    .detect	     = mir3da_detect,
#if !defined(CONFIG_HAS_EARLYSUSPEND)        
    .suspend     = mir3da_suspend,
    .resume      = mir3da_resume,
#endif
    .id_table    = mir3da_id,
#if MTK_ANDROID_23
    .address_data = &mir3da_addr_data,    
#endif
};

/*----------------------------------------------------------------------------*/
#if MTK_AUTO_MODE
static int mir3da_local_init(void) 
{
    struct acc_hw *hw = mir3da_get_cust_acc_hw();
	
    MI_FUN;

    mir3da_power(hw, 1);

    if(i2c_add_driver(&mir3da_driver))
    {
        MI_ERR("add driver error\n");
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static int mir3da_local_remove(void)
{
    struct acc_hw *hw =mir3da_get_cust_acc_hw();

    MI_FUN;    
	
    mir3da_power(hw, 0);    
	
    i2c_del_driver(&mir3da_driver);
	
    return 0;
}
/*----------------------------------------------------------------------------*/
#else
static int mir3da_platform_probe(struct platform_device *pdev) 
{
    struct acc_hw *hw = mir3da_get_cust_acc_hw();

    MI_FUN;

    mir3da_power(hw, 1);

    if(i2c_add_driver(&mir3da_driver))
    {
        MI_ERR("add driver error\n");
        return -1;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static int mir3da_platform_remove(struct platform_device *pdev)
{
    struct acc_hw *hw = mir3da_get_cust_acc_hw();

    MI_FUN;    

    mir3da_power(hw, 0);
	
    i2c_del_driver(&mir3da_driver);

    return 0;
}
#endif
/*----------------------------------------------------------------------------*/
static int __init mir3da_init(void)
{    
    struct acc_hw *hw = mir3da_get_cust_acc_hw();
    
    MI_FUN;

#if !MTK_ANDROID_23
    i2c_register_board_info(hw->i2c_num, &mir3da_i2c_boardinfo, 1);
#endif

#if MTK_AUTO_MODE
    hwmsen_gsensor_add(&mir3da_init_info);
#else
    if(platform_driver_register(&mir3da_gsensor_driver))
    {
        MI_ERR("failed to register driver");
        return -ENODEV;
    }
#endif

    return 0;
}
/*----------------------------------------------------------------------------*/
static void __exit mir3da_exit(void)
{    
    MI_FUN;
	
#if !MTK_AUTO_MODE
    platform_driver_unregister(&mir3da_gsensor_driver);
#endif
}
/*----------------------------------------------------------------------------*/

MODULE_AUTHOR("MiraMEMS <lschen@miramems.com>");
MODULE_DESCRIPTION("MirMEMS 3-Axis Accelerometer driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

module_init(mir3da_init);
module_exit(mir3da_exit);
