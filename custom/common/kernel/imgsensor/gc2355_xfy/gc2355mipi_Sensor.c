/*****************************************************************************
 *
 * Filename:
 * ---------
 *   sensor.c
 *
 * Project:
 * --------
 *   RAW
 *
 * Description:
 * ------------
 *   Source code of Sensor driver
 *
 *
 * Author:
 * -------
 *   Leo Lee
 *
 *============================================================================
 *             HISTORY
 *------------------------------------------------------------------------------
 * $Revision:$
 * $Modtime:$
 * $Log:$
 *
 * 04 10  2013
 * First release MT6589 GC2355MIPI driver Version 1.0
 *
 *------------------------------------------------------------------------------
 *============================================================================
 ****************************************************************************/

#include <linux/videodev2.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <asm/atomic.h>

#include "kd_camera_hw.h"
#include "kd_imgsensor.h"
#include "kd_imgsensor_define.h"
#include "kd_imgsensor_errcode.h"

#include "gc2355mipi_Sensor.h"
#include "gc2355mipi_Camera_Sensor_para.h"
#include "gc2355mipi_CameraCustomized.h"

#ifdef GC2355MIPI_DEBUG
#define SENSORDB printk
#else
#define SENSORDB(x,...)
#endif

#if defined(MT6577)||defined(MT6589)
static DEFINE_SPINLOCK(gc2355mipi_drv_lock);
#endif
//static kal_uint16 Half_iShutter=0;
static kal_bool b_Enable_Half = false;
extern int iReadRegI2C(u8 *a_pSendData , u16 a_sizeSendData, u8 * a_pRecvData, u16 a_sizeRecvData, u16 i2cId);
extern int iWriteRegI2C(u8 *a_pSendData , u16 a_sizeSendData, u16 i2cId);
//#define GC2235MIPI_TEST_PATTERN_CHECKSUM (0x1a884aad)
#define GC2235MIPI_TEST_PATTERN_CHECKSUM (0xf77e69f) //(0x3ef10f8f)

static GC2355MIPI_sensor_struct GC2355MIPI_sensor =
{
	.eng_info =
	{
		.SensorId = 128,
		.SensorType = CMOS_SENSOR,
		.SensorOutputDataFormat = GC2355MIPI_COLOR_FORMAT,
	},
	.Mirror = GC2355MIPI_IMAGE_V_MIRROR,
	.shutter = 0x20,  
	.gain = 0x20,
	.pclk = GC2355MIPI_PREVIEW_CLK,
	.frame_height = GC2355MIPI_PV_PERIOD_LINE_NUMS,
	.line_length = GC2355MIPI_PV_PERIOD_PIXEL_NUMS,
};


/*************************************************************************
* FUNCTION
*    GC2355MIPI_write_cmos_sensor
*
* DESCRIPTION
*    This function wirte data to CMOS sensor through I2C
*
* PARAMETERS
*    addr: the 16bit address of register
*    para: the 8bit value of register
*
* RETURNS 
*    None
*
* LOCAL AFFECTED
*
*************************************************************************/
static void GC2355MIPI_write_cmos_sensor(kal_uint8 addr, kal_uint8 para)
{
kal_uint8 out_buff[2];

    out_buff[0] = addr;
    out_buff[1] = para;

    iWriteRegI2C((u8*)out_buff , (u16)sizeof(out_buff), GC2355MIPI_WRITE_ID); 
}

/*************************************************************************
* FUNCTION
*    GC2035_read_cmos_sensor
*
* DESCRIPTION
*    This function read data from CMOS sensor through I2C.
*
* PARAMETERS
*    addr: the 16bit address of register
*
* RETURNS
*    8bit data read through I2C
*
* LOCAL AFFECTED
*
*************************************************************************/
static kal_uint8 GC2355MIPI_read_cmos_sensor(kal_uint8 addr)
{
  kal_uint8 in_buff[1] = {0xFF};
  kal_uint8 out_buff[1];
  
  out_buff[0] = addr;

    if (0 != iReadRegI2C((u8*)out_buff , (u16) sizeof(out_buff), (u8*)in_buff, (u16) sizeof(in_buff), GC2355MIPI_WRITE_ID)) {
        SENSORDB("ERROR: GC2355MIPI_read_cmos_sensor \n");
    }
  return in_buff[0];
}

/*************************************************************************
* FUNCTION
*	GC2355MIPI_SetShutter
*
* DESCRIPTION
*	This function set e-shutter of GC2355MIPI to change exposure time.
*
* PARAMETERS
*   iShutter : exposured lines
*
* RETURNS
*	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
void GC2355MIPI_set_shutter(kal_uint16 iShutter)
{
#if defined(MT6577)||defined(MT6589)
	spin_lock(&gc2355mipi_drv_lock);
#endif
	GC2355MIPI_sensor.shutter = iShutter;
#if defined(MT6577)||defined(MT6589)
	spin_unlock(&gc2355mipi_drv_lock);
#endif

	if (!iShutter) iShutter = 1; /* avoid 0 */
	
#ifdef GC2355MIPI_DRIVER_TRACE
	SENSORDB("GC2355MIPI_set_shutter iShutter = %d \n",iShutter);
#endif
	if(iShutter < 1) iShutter = 1;
	if(iShutter > 16383) iShutter = 16383;//2^14
	//Update Shutter
	GC2355MIPI_write_cmos_sensor(0x04, (iShutter) & 0xFF);
	GC2355MIPI_write_cmos_sensor(0x03, (iShutter >> 8) & 0x3F);	
}   /*  Set_GC2355MIPI_Shutter */

/*************************************************************************
* FUNCTION
*	GC2355MIPI_SetGain
*
* DESCRIPTION
*	This function is to set global gain to sensor.
*
* PARAMETERS
*   iGain : sensor global gain(base: 0x40)
*
* RETURNS 
*	the actually gain set to sensor.
*
* GLOBALS AFFECTED
*
*************************************************************************/
#define ANALOG_GAIN_1 64  // 1.00x
#define ANALOG_GAIN_2 88  // 1.375x
#define ANALOG_GAIN_3 122  // 1.90x
#define ANALOG_GAIN_4 168  // 2.625x
#define ANALOG_GAIN_5 239  // 3.738x
#define ANALOG_GAIN_6 330  // 5.163x
#define ANALOG_GAIN_7 470  // 7.350x

kal_uint16  GC2355MIPI_SetGain(kal_uint16 iGain)
{
		kal_uint16 iReg,temp,temp1,luma_value;
		
	#ifdef GC2355MIPI_DRIVER_TRACE
	SENSORDB("GC2355MIPI_SetGain iGain = %d \n",iGain);
	#endif
	GC2355MIPI_write_cmos_sensor(0xb1, 0x01);
	GC2355MIPI_write_cmos_sensor(0xb2, 0x00);

	iReg = iGain;
	if(iReg < 0x40)
		iReg = 0x40;
	if((ANALOG_GAIN_1<= iReg)&&(iReg < ANALOG_GAIN_2))
	{
		//analog gain
		GC2355MIPI_write_cmos_sensor(0xb6,  0x00);// 
		temp = iReg;
		GC2355MIPI_write_cmos_sensor(0xb1, temp>>6);
		GC2355MIPI_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 1x , add pregain = %d\n",temp);
	
	}
	else if((ANALOG_GAIN_2<= iReg)&&(iReg < ANALOG_GAIN_3))
	{
		//analog gain
		GC2355MIPI_write_cmos_sensor(0xb6,  0x01);// 
		temp = 64*iReg/ANALOG_GAIN_2;
		GC2355MIPI_write_cmos_sensor(0xb1, temp>>6);
		GC2355MIPI_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 1.375x , add pregain = %d\n",temp);
	}

	else if((ANALOG_GAIN_3<= iReg)&&(iReg < ANALOG_GAIN_4))
	{
		//analog gain
		GC2355MIPI_write_cmos_sensor(0xb6,  0x02);//
		temp = 64*iReg/ANALOG_GAIN_3;
		GC2355MIPI_write_cmos_sensor(0xb1, temp>>6);
		GC2355MIPI_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 1.90x , add pregain = %d\n",temp);
	}

	else if(ANALOG_GAIN_4<= iReg)
	{
		//analog gain
		GC2355MIPI_write_cmos_sensor(0xb6,  0x03);//
		temp = 64*iReg/ANALOG_GAIN_4;
		GC2355MIPI_write_cmos_sensor(0xb1, temp>>6);
		GC2355MIPI_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 2.625x" , add pregain = %d\n",temp);
	}
/*	else if((ANALOG_GAIN_4<= iReg)&&(iReg < ANALOG_GAIN_5))
	{
		//analog gain
		GC2355_write_cmos_sensor(0xb6,  0x03);//
		temp = 64*iReg/ANALOG_GAIN_4;
		GC2355_write_cmos_sensor(0xb1, temp>>6);
		GC2355_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 2.625x , add pregain = %d\n",temp);
	}

	else if((ANALOG_GAIN_5<= iReg)&&(iReg)&&(iReg < ANALOG_GAIN_6))
	{
		//analog gain
		GC2355_write_cmos_sensor(0xb6,  0x04);//
		temp = 64*iReg/ANALOG_GAIN_5;
		GC2355_write_cmos_sensor(0xb1, temp>>6);
		GC2355_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 3.738x , add pregain = %d\n",temp);
	}

	else if((ANALOG_GAIN_6<= iReg)&&(iReg < ANALOG_GAIN_7))
	{
		//analog gain
		GC2355_write_cmos_sensor(0xb6,  0x05);//
		temp = 64*iReg/ANALOG_GAIN_6;
		GC2355_write_cmos_sensor(0xb1, temp>>6);
		GC2355_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 5.163x , add pregain = %d\n",temp);
	}
	else if(ANALOG_GAIN_7<= iReg)
	{
		//analog gain
		GC2355_write_cmos_sensor(0xb6,  0x06);//
		temp = 64*iReg/ANALOG_GAIN_7;
		GC2355_write_cmos_sensor(0xb1, temp>>6);
		GC2355_write_cmos_sensor(0xb2, (temp<<2)&0xfc);
		//SENSORDB("GC2355 analogic gain 7.350x" , add pregain = %d\n",temp);
	}
#endif*/
	
	return iGain;
}
/*************************************************************************
* FUNCTION
*	GC2355MIPI_NightMode
*
* DESCRIPTION
*	This function night mode of GC2355MIPI.
*
* PARAMETERS
*	bEnable: KAL_TRUE -> enable night mode, otherwise, disable night mode
*
* RETURNS
*	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
void GC2355MIPI_night_mode(kal_bool enable)
{
/*No Need to implement this function*/
#if 0 
	const kal_uint16 dummy_pixel = GC2355MIPI_sensor.line_length - GC2355MIPI_PV_PERIOD_PIXEL_NUMS;
	const kal_uint16 pv_min_fps =  enable ? GC2355MIPI_sensor.night_fps : GC2355MIPI_sensor.normal_fps;
	kal_uint16 dummy_line = GC2355MIPI_sensor.frame_height - GC2355MIPI_PV_PERIOD_LINE_NUMS;
	kal_uint16 max_exposure_lines;
	
	printk("[GC2355MIPI_night_mode]enable=%d",enable);
	if (!GC2355MIPI_sensor.video_mode) return;
	max_exposure_lines = GC2355MIPI_sensor.pclk * GC2355MIPI_FPS(1) / (pv_min_fps * GC2355MIPI_sensor.line_length);
	if (max_exposure_lines > GC2355MIPI_sensor.frame_height) /* fix max frame rate, AE table will fix min frame rate */
//	{
//	  dummy_line = max_exposure_lines - GC2355MIPI_PV_PERIOD_LINE_NUMS;
//	}
#endif
}   /*  GC2355MIPI_NightMode    */


/* write camera_para to sensor register */
static void GC2355MIPI_camera_para_to_sensor(void)
{
  kal_uint32 i;
#ifdef GC2355MIPI_DRIVER_TRACE
	 SENSORDB("GC2355MIPI_camera_para_to_sensor\n");
#endif
  for (i = 0; 0xFFFFFFFF != GC2355MIPI_sensor.eng.reg[i].Addr; i++)
  {
    GC2355MIPI_write_cmos_sensor(GC2355MIPI_sensor.eng.reg[i].Addr, GC2355MIPI_sensor.eng.reg[i].Para);
  }
  for (i = GC2355MIPI_FACTORY_START_ADDR; 0xFFFFFFFF != GC2355MIPI_sensor.eng.reg[i].Addr; i++)
  {
    GC2355MIPI_write_cmos_sensor(GC2355MIPI_sensor.eng.reg[i].Addr, GC2355MIPI_sensor.eng.reg[i].Para);
  }
  GC2355MIPI_SetGain(GC2355MIPI_sensor.gain); /* update gain */
}

/* update camera_para from sensor register */
static void GC2355MIPI_sensor_to_camera_para(void)
{
  kal_uint32 i,temp_data;
#ifdef GC2355MIPI_DRIVER_TRACE
   SENSORDB("GC2355MIPI_sensor_to_camera_para\n");
#endif
  for (i = 0; 0xFFFFFFFF != GC2355MIPI_sensor.eng.reg[i].Addr; i++)
  {
  	temp_data = GC2355MIPI_read_cmos_sensor(GC2355MIPI_sensor.eng.reg[i].Addr);
#if defined(MT6577)||defined(MT6589)
	spin_lock(&gc2355mipi_drv_lock);
#endif
    GC2355MIPI_sensor.eng.reg[i].Para = temp_data;
#if defined(MT6577)||defined(MT6589)
	spin_unlock(&gc2355mipi_drv_lock);
#endif
  }
  for (i = GC2355MIPI_FACTORY_START_ADDR; 0xFFFFFFFF != GC2355MIPI_sensor.eng.reg[i].Addr; i++)
  {
  	temp_data = GC2355MIPI_read_cmos_sensor(GC2355MIPI_sensor.eng.reg[i].Addr);
#if defined(MT6577)||defined(MT6589)
	spin_lock(&gc2355mipi_drv_lock);
#endif
    GC2355MIPI_sensor.eng.reg[i].Para = temp_data;
#if defined(MT6577)||defined(MT6589)
	spin_unlock(&gc2355mipi_drv_lock);
#endif
  }
}

/* ------------------------ Engineer mode ------------------------ */
inline static void GC2355MIPI_get_sensor_group_count(kal_int32 *sensor_count_ptr)
{
#ifdef GC2355MIPI_DRIVER_TRACE
   SENSORDB("GC2355MIPI_get_sensor_group_count\n");
#endif
  *sensor_count_ptr = GC2355MIPI_GROUP_TOTAL_NUMS;
}

inline static void GC2355MIPI_get_sensor_group_info(MSDK_SENSOR_GROUP_INFO_STRUCT *para)
{
#ifdef GC2355MIPI_DRIVER_TRACE
   SENSORDB("GC2355MIPI_get_sensor_group_info\n");
#endif
  switch (para->GroupIdx)
  {
  case GC2355MIPI_PRE_GAIN:
    sprintf(para->GroupNamePtr, "CCT");
    para->ItemCount = 5;
    break;
  case GC2355MIPI_CMMCLK_CURRENT:
    sprintf(para->GroupNamePtr, "CMMCLK Current");
    para->ItemCount = 1;
    break;
  case GC2355MIPI_FRAME_RATE_LIMITATION:
    sprintf(para->GroupNamePtr, "Frame Rate Limitation");
    para->ItemCount = 2;
    break;
  case GC2355MIPI_REGISTER_EDITOR:
    sprintf(para->GroupNamePtr, "Register Editor");
    para->ItemCount = 2;
    break;
  default:
    ASSERT(0);
  }
}

inline static void GC2355MIPI_get_sensor_item_info(MSDK_SENSOR_ITEM_INFO_STRUCT *para)
{

  const static kal_char *cct_item_name[] = {"SENSOR_BASEGAIN", "Pregain-R", "Pregain-Gr", "Pregain-Gb", "Pregain-B"};
  const static kal_char *editer_item_name[] = {"REG addr", "REG value"};
  
#ifdef GC2355MIPI_DRIVER_TRACE
	 SENSORDB("GC2355MIPI_get_sensor_item_info\n");
#endif
  switch (para->GroupIdx)
  {
  case GC2355MIPI_PRE_GAIN:
    switch (para->ItemIdx)
    {
    case GC2355MIPI_SENSOR_BASEGAIN:
    case GC2355MIPI_PRE_GAIN_R_INDEX:
    case GC2355MIPI_PRE_GAIN_Gr_INDEX:
    case GC2355MIPI_PRE_GAIN_Gb_INDEX:
    case GC2355MIPI_PRE_GAIN_B_INDEX:
      break;
    default:
      ASSERT(0);
    }
    sprintf(para->ItemNamePtr, cct_item_name[para->ItemIdx - GC2355MIPI_SENSOR_BASEGAIN]);
    para->ItemValue = GC2355MIPI_sensor.eng.cct[para->ItemIdx].Para * 1000 / BASEGAIN;
    para->IsTrueFalse = para->IsReadOnly = para->IsNeedRestart = KAL_FALSE;
    para->Min = GC2355MIPI_MIN_ANALOG_GAIN * 1000;
    para->Max = GC2355MIPI_MAX_ANALOG_GAIN * 1000;
    break;
  case GC2355MIPI_CMMCLK_CURRENT:
    switch (para->ItemIdx)
    {
    case 0:
      sprintf(para->ItemNamePtr, "Drv Cur[2,4,6,8]mA");
      switch (GC2355MIPI_sensor.eng.reg[GC2355MIPI_CMMCLK_CURRENT_INDEX].Para)
      {
      case ISP_DRIVING_2MA:
        para->ItemValue = 2;
        break;
      case ISP_DRIVING_4MA:
        para->ItemValue = 4;
        break;
      case ISP_DRIVING_6MA:
        para->ItemValue = 6;
        break;
      case ISP_DRIVING_8MA:
        para->ItemValue = 8;
        break;
      default:
        ASSERT(0);
      }
      para->IsTrueFalse = para->IsReadOnly = KAL_FALSE;
      para->IsNeedRestart = KAL_TRUE;
      para->Min = 2;
      para->Max = 8;
      break;
    default:
      ASSERT(0);
    }
    break;
  case GC2355MIPI_FRAME_RATE_LIMITATION:
    switch (para->ItemIdx)
    {
    case 0:
      sprintf(para->ItemNamePtr, "Max Exposure Lines");
      para->ItemValue = 5998;
      break;
    case 1:
      sprintf(para->ItemNamePtr, "Min Frame Rate");
      para->ItemValue = 5;
      break;
    default:
      ASSERT(0);
    }
    para->IsTrueFalse = para->IsNeedRestart = KAL_FALSE;
    para->IsReadOnly = KAL_TRUE;
    para->Min = para->Max = 0;
    break;
  case GC2355MIPI_REGISTER_EDITOR:
    switch (para->ItemIdx)
    {
    case 0:
    case 1:
      sprintf(para->ItemNamePtr, editer_item_name[para->ItemIdx]);
      para->ItemValue = 0;
      para->IsTrueFalse = para->IsReadOnly = para->IsNeedRestart = KAL_FALSE;
      para->Min = 0;
      para->Max = (para->ItemIdx == 0 ? 0xFFFF : 0xFF);
      break;
    default:
      ASSERT(0);
    }
    break;
  default:
    ASSERT(0);
  }
}

inline static kal_bool GC2355MIPI_set_sensor_item_info(MSDK_SENSOR_ITEM_INFO_STRUCT *para)
{
  kal_uint16 temp_para;
#ifdef GC2355MIPI_DRIVER_TRACE
   SENSORDB("GC2355MIPI_set_sensor_item_info\n");
#endif
  switch (para->GroupIdx)
  {
  case GC2355MIPI_PRE_GAIN:
    switch (para->ItemIdx)
    {
    case GC2355MIPI_SENSOR_BASEGAIN:
    case GC2355MIPI_PRE_GAIN_R_INDEX:
    case GC2355MIPI_PRE_GAIN_Gr_INDEX:
    case GC2355MIPI_PRE_GAIN_Gb_INDEX:
    case GC2355MIPI_PRE_GAIN_B_INDEX:
#if defined(MT6577)||defined(MT6589)
		spin_lock(&gc2355mipi_drv_lock);
#endif
      GC2355MIPI_sensor.eng.cct[para->ItemIdx].Para = para->ItemValue * BASEGAIN / 1000;
#if defined(MT6577)||defined(MT6589)
	  spin_unlock(&gc2355mipi_drv_lock);
#endif
      GC2355MIPI_SetGain(GC2355MIPI_sensor.gain); /* update gain */
      break;
    default:
      ASSERT(0);
    }
    break;
  case GC2355MIPI_CMMCLK_CURRENT:
    switch (para->ItemIdx)
    {
    case 0:
      switch (para->ItemValue)
      {
      case 2:
        temp_para = ISP_DRIVING_2MA;
        break;
      case 3:
      case 4:
        temp_para = ISP_DRIVING_4MA;
        break;
      case 5:
      case 6:
        temp_para = ISP_DRIVING_6MA;
        break;
      default:
        temp_para = ISP_DRIVING_8MA;
        break;
      }
      //GC2355MIPI_set_isp_driving_current(temp_para);
#if defined(MT6577)||defined(MT6589)
      spin_lock(&gc2355mipi_drv_lock);
#endif
      GC2355MIPI_sensor.eng.reg[GC2355MIPI_CMMCLK_CURRENT_INDEX].Para = temp_para;
#if defined(MT6577)||defined(MT6589)
	  spin_unlock(&gc2355mipi_drv_lock);
#endif
      break;
    default:
      ASSERT(0);
    }
    break;
  case GC2355MIPI_FRAME_RATE_LIMITATION:
    ASSERT(0);
    break;
  case GC2355MIPI_REGISTER_EDITOR:
    switch (para->ItemIdx)
    {
      static kal_uint32 fac_sensor_reg;
    case 0:
      if (para->ItemValue < 0 || para->ItemValue > 0xFFFF) return KAL_FALSE;
      fac_sensor_reg = para->ItemValue;
      break;
    case 1:
      if (para->ItemValue < 0 || para->ItemValue > 0xFF) return KAL_FALSE;
      GC2355MIPI_write_cmos_sensor(fac_sensor_reg, para->ItemValue);
      break;
    default:
      ASSERT(0);
    }
    break;
  default:
    ASSERT(0);
  }
  return KAL_TRUE;
}

void GC2355MIPI_SetMirrorFlip(GC2355MIPI_IMAGE_MIRROR Mirror)
{
/*	switch(Mirror)
	{
		case GC2355MIPI_IMAGE_NORMAL://IMAGE_V_MIRROR:
		   GC2355MIPI_write_cmos_sensor(0x17,0x14);
		   GC2355MIPI_write_cmos_sensor(0x92,0x03);
		   GC2355MIPI_write_cmos_sensor(0x94,0x07);
		    break;
		case GC2355MIPI_IMAGE_H_MIRROR://IMAGE_NORMAL:
		   GC2355MIPI_write_cmos_sensor(0x17,0x15);
		   GC2355MIPI_write_cmos_sensor(0x92,0x03);
		   GC2355MIPI_write_cmos_sensor(0x94,0x06);
		    break;
		case GC2355MIPI_IMAGE_V_MIRROR://IMAGE_HV_MIRROR:
		   GC2355MIPI_write_cmos_sensor(0x17,0x16);
		   GC2355MIPI_write_cmos_sensor(0x92,0x02);
		   GC2355MIPI_write_cmos_sensor(0x94,0x07);
		    break;
		case GC2355MIPI_IMAGE_HV_MIRROR://IMAGE_H_MIRROR:
		   GC2355MIPI_write_cmos_sensor(0x17,0x17);
		   GC2355MIPI_write_cmos_sensor(0x92,0x02);
		   GC2355MIPI_write_cmos_sensor(0x94,0x06);
		    break;
	}*/
}

static void GC2355MIPI_Sensor_Init(void)
{
	/////////////////////////////////////////////////////
	//////////////////////	 SYS   //////////////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0xfe,0x80);
	GC2355MIPI_write_cmos_sensor(0xfe,0x80);
	GC2355MIPI_write_cmos_sensor(0xfe,0x80);
	GC2355MIPI_write_cmos_sensor(0xf2,0x00); //sync_pad_io_ebi
	GC2355MIPI_write_cmos_sensor(0xf6,0x00); //up down
#if defined(GC2355MIPI_1Lane)
	GC2355MIPI_write_cmos_sensor(0xf7,0x19); //PLL enable
#else
	GC2355MIPI_write_cmos_sensor(0xf7,0x31); //PLL enable
#endif
	GC2355MIPI_write_cmos_sensor(0xf8,0x06); //PLL mode 2
	GC2355MIPI_write_cmos_sensor(0xf9,0x0e); //[0]PLL enable
	GC2355MIPI_write_cmos_sensor(0xfa,0x00); //div
	GC2355MIPI_write_cmos_sensor(0xfc,0x06); 
	GC2355MIPI_write_cmos_sensor(0xfe,0x00);
	
	/////////////////////////////////////////////////////
	///////////////    ANALOG & CISCTL    ///////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0x03,0x07); 
	GC2355MIPI_write_cmos_sensor(0x04,0xd0); 
	GC2355MIPI_write_cmos_sensor(0x05,0x03); //max 20fps
	GC2355MIPI_write_cmos_sensor(0x06,0x4c); 
	GC2355MIPI_write_cmos_sensor(0x07,0x00);
	GC2355MIPI_write_cmos_sensor(0x08,0x12); 
	GC2355MIPI_write_cmos_sensor(0x0a,0x00); //row start
	GC2355MIPI_write_cmos_sensor(0x0c,0x04); //0c//col start
	GC2355MIPI_write_cmos_sensor(0x0d,0x04);
	GC2355MIPI_write_cmos_sensor(0x0e,0xc0); 
	GC2355MIPI_write_cmos_sensor(0x0f,0x06); 
	GC2355MIPI_write_cmos_sensor(0x10,0x50); //Window setting 1616x1216
	GC2355MIPI_write_cmos_sensor(0x17,0x14);//0x17
	GC2355MIPI_write_cmos_sensor(0x19,0x0b);
	GC2355MIPI_write_cmos_sensor(0x1b,0x49); 
	GC2355MIPI_write_cmos_sensor(0x1c,0x12);
	GC2355MIPI_write_cmos_sensor(0x1d,0x10); 
	GC2355MIPI_write_cmos_sensor(0x1e,0xbc); //col_r/rowclk_mode/rsthigh_en FPN
	GC2355MIPI_write_cmos_sensor(0x1f,0xc8); //rsgl_s_mode/vpix_s_mode
	GC2355MIPI_write_cmos_sensor(0x20,0x71);
	GC2355MIPI_write_cmos_sensor(0x21,0x20); //rsg
	GC2355MIPI_write_cmos_sensor(0x22,0xa0); 
	GC2355MIPI_write_cmos_sensor(0x23,0x51); 
	GC2355MIPI_write_cmos_sensor(0x24,0x19); //drv
	GC2355MIPI_write_cmos_sensor(0x27,0x20);
	GC2355MIPI_write_cmos_sensor(0x28,0x00);
	GC2355MIPI_write_cmos_sensor(0x2b,0x81); //sf_s_mode FPN
	GC2355MIPI_write_cmos_sensor(0x2c,0x38); //ispg FPN
	GC2355MIPI_write_cmos_sensor(0x2e,0x16); //eq width
	GC2355MIPI_write_cmos_sensor(0x2f,0x14); 
	GC2355MIPI_write_cmos_sensor(0x30,0x00);
	GC2355MIPI_write_cmos_sensor(0x31,0x01);
	GC2355MIPI_write_cmos_sensor(0x32,0x02);
	GC2355MIPI_write_cmos_sensor(0x33,0x03);
	GC2355MIPI_write_cmos_sensor(0x34,0x07);
	GC2355MIPI_write_cmos_sensor(0x35,0x0b);
	GC2355MIPI_write_cmos_sensor(0x36,0x0f);

	/////////////////////////////////////////////////////
	//////////////////////	 gain   /////////////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0xb0,0x50);
	GC2355MIPI_write_cmos_sensor(0xb1,0x01);
	GC2355MIPI_write_cmos_sensor(0xb2,0x00);
	GC2355MIPI_write_cmos_sensor(0xb3,0x40);
	GC2355MIPI_write_cmos_sensor(0xb4,0x40);
	GC2355MIPI_write_cmos_sensor(0xb5,0x40);
	GC2355MIPI_write_cmos_sensor(0xb6,0x00);

	/////////////////////////////////////////////////////
	//////////////////////   crop   /////////////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0x92,0x02);
	GC2355MIPI_write_cmos_sensor(0x94,0x00);
	GC2355MIPI_write_cmos_sensor(0x95,0x04);
	GC2355MIPI_write_cmos_sensor(0x96,0xb0);
	GC2355MIPI_write_cmos_sensor(0x97,0x06);
	GC2355MIPI_write_cmos_sensor(0x98,0x40); //out window set 1600x1200

	/////////////////////////////////////////////////////
	//////////////////////    BLK   /////////////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0x18,0x02);
	GC2355MIPI_write_cmos_sensor(0x1a,0x01);
	GC2355MIPI_write_cmos_sensor(0x40,0x42);
	GC2355MIPI_write_cmos_sensor(0x41,0x00); 
	GC2355MIPI_write_cmos_sensor(0x44,0x00); 
	GC2355MIPI_write_cmos_sensor(0x45,0x00);
	GC2355MIPI_write_cmos_sensor(0x46,0x00);	
	GC2355MIPI_write_cmos_sensor(0x47,0x00); 
	GC2355MIPI_write_cmos_sensor(0x48,0x00); 
	GC2355MIPI_write_cmos_sensor(0x49,0x00);
	GC2355MIPI_write_cmos_sensor(0x4a,0x00);	
	GC2355MIPI_write_cmos_sensor(0x4b,0x00);	
	GC2355MIPI_write_cmos_sensor(0x4e,0x3c); 
	GC2355MIPI_write_cmos_sensor(0x4f,0x00); 
	GC2355MIPI_write_cmos_sensor(0x5e,0x00);
	GC2355MIPI_write_cmos_sensor(0x66,0x20);
	GC2355MIPI_write_cmos_sensor(0x6a,0x03); //manual offset  
	GC2355MIPI_write_cmos_sensor(0x6b,0x03);
	GC2355MIPI_write_cmos_sensor(0x6c,0x03);///melody
	GC2355MIPI_write_cmos_sensor(0x6d,0x03);
	GC2355MIPI_write_cmos_sensor(0x6e,0x03);
	GC2355MIPI_write_cmos_sensor(0x6f,0x03);
	GC2355MIPI_write_cmos_sensor(0x70,0x03);
	GC2355MIPI_write_cmos_sensor(0x71,0x03);

	/////////////////////////////////////////////////////
	////////////////////  dark sun  /////////////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0x87,0x03); 
	GC2355MIPI_write_cmos_sensor(0xe0,0xe7); //dark sun en
	GC2355MIPI_write_cmos_sensor(0xe3,0xc0); 

	/////////////////////////////////////////////////////
	//////////////////////   MIPI   /////////////////////
	/////////////////////////////////////////////////////
	GC2355MIPI_write_cmos_sensor(0xfe, 0x03);
	GC2355MIPI_write_cmos_sensor(0x02, 0x00);
	GC2355MIPI_write_cmos_sensor(0x03, 0x90);
	GC2355MIPI_write_cmos_sensor(0x04, 0x01);
	GC2355MIPI_write_cmos_sensor(0x05, 0x00);
	GC2355MIPI_write_cmos_sensor(0x06, 0xa2);
	GC2355MIPI_write_cmos_sensor(0x11, 0x2b);
	GC2355MIPI_write_cmos_sensor(0x12, 0xd0); 
	GC2355MIPI_write_cmos_sensor(0x13, 0x07); 
	GC2355MIPI_write_cmos_sensor(0x15, 0x60);

	GC2355MIPI_write_cmos_sensor(0x21, 0x10);
	GC2355MIPI_write_cmos_sensor(0x24, 0x02);
	GC2355MIPI_write_cmos_sensor(0x26, 0x08);
	GC2355MIPI_write_cmos_sensor(0x27, 0x06);
#if defined(GC2355MIPI_1Lane)
	GC2355MIPI_write_cmos_sensor(0x01, 0x83);
	GC2355MIPI_write_cmos_sensor(0x10, 0x90);
	GC2355MIPI_write_cmos_sensor(0x22, 0x05);
	GC2355MIPI_write_cmos_sensor(0x23, 0x30);
	GC2355MIPI_write_cmos_sensor(0x25, 0x02);
	GC2355MIPI_write_cmos_sensor(0x29, 0x06);
#else
	GC2355MIPI_write_cmos_sensor(0x01, 0x87);
	GC2355MIPI_write_cmos_sensor(0x10, 0x91);
	GC2355MIPI_write_cmos_sensor(0x22, 0x03);
	GC2355MIPI_write_cmos_sensor(0x23, 0x20);
	GC2355MIPI_write_cmos_sensor(0x25, 0x10);
	GC2355MIPI_write_cmos_sensor(0x29, 0x02);
#endif
	GC2355MIPI_write_cmos_sensor(0x2a, 0x0a); 
	GC2355MIPI_write_cmos_sensor(0x2b, 0x08);
	GC2355MIPI_write_cmos_sensor(0x40, 0x00);
	GC2355MIPI_write_cmos_sensor(0x41, 0x00);    
	GC2355MIPI_write_cmos_sensor(0x42, 0x40);
	GC2355MIPI_write_cmos_sensor(0x43, 0x06);  
	GC2355MIPI_write_cmos_sensor(0xfe, 0x00);

}   /*  GC2355MIPI_Sensor_Init  */

/*****************************************************************************/
/* Windows Mobile Sensor Interface */
/*****************************************************************************/
/*************************************************************************
* FUNCTION
*	GC2355MIPIOpen
*
* DESCRIPTION
*	This function initialize the registers of CMOS sensor
*
* PARAMETERS
*	None
*
* RETURNS
*	None
*
* GLOBALS AFFECTED
*
*************************************************************************/

UINT32 GC2355MIPIOpen(void)
{
	kal_uint16 sensor_id=0; 

	// check if sensor ID correct
	sensor_id=((GC2355MIPI_read_cmos_sensor(0xf0) << 8) | GC2355MIPI_read_cmos_sensor(0xf1));   
#ifdef GC2355MIPI_DRIVER_TRACE
	SENSORDB("GC2355MIPIOpen, sensor_id:%x \n",sensor_id);
#endif		
	if (sensor_id != GC2355MIPI_SENSOR_ID)
		return ERROR_SENSOR_CONNECT_FAIL;
	
	/* initail sequence write in  */
	GC2355MIPI_Sensor_Init();

//	GC2355MIPI_SetMirrorFlip(GC2355MIPI_sensor.Mirror);

	return ERROR_NONE;
}   /* GC2355MIPIOpen  */

/*************************************************************************
* FUNCTION
*   GC2355MIPIGetSensorID
*
* DESCRIPTION
*   This function get the sensor ID 
*
* PARAMETERS
*   *sensorID : return the sensor ID 
*
* RETURNS
*   None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 GC2355MIPIGetSensorID(UINT32 *sensorID) 
{
	// check if sensor ID correct
	*sensorID=((GC2355MIPI_read_cmos_sensor(0xf0) << 8) | GC2355MIPI_read_cmos_sensor(0xf1));	
#ifdef GC2355MIPI_DRIVER_TRACE
	SENSORDB("GC2355MIPIGetSensorID:%x \n",*sensorID);
#endif		
	if (*sensorID != GC2355MIPI_SENSOR_ID) {		
		*sensorID = 0xFFFFFFFF;		
		return ERROR_SENSOR_CONNECT_FAIL;
	}
   return ERROR_NONE;
}

/*************************************************************************
* FUNCTION
*	GC2355MIPIClose
*
* DESCRIPTION
*	This function is to turn off sensor module power.
*
* PARAMETERS
*	None
*
* RETURNS
*	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 GC2355MIPIClose(void)
{
#ifdef GC2355MIPI_DRIVER_TRACE
   SENSORDB("GC2355MIPIClose\n");
#endif
  //kdCISModulePowerOn(SensorIdx,currSensorName,0,mode_name);
//	DRV_I2CClose(GC2355MIPIhDrvI2C);
	return ERROR_NONE;
}   /* GC2355MIPIClose */

/*************************************************************************
* FUNCTION
* GC2355MIPIPreview
*
* DESCRIPTION
*	This function start the sensor preview.
*
* PARAMETERS
*	*image_window : address pointer of pixel numbers in one period of HSYNC
*  *sensor_config_data : address pointer of line numbers in one period of VSYNC
*
* RETURNS
*	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 GC2355MIPIPreview(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
					  MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	
#if defined(MT6577)||defined(MT6589)
	spin_lock(&gc2355mipi_drv_lock);	
#endif
	GC2355MIPI_sensor.pv_mode = KAL_TRUE;
	
	//GC2355MIPI_set_mirror(sensor_config_data->SensorImageMirror);
	switch (sensor_config_data->SensorOperationMode)
	{
	  case MSDK_SENSOR_OPERATION_MODE_VIDEO: 	  	
		GC2355MIPI_sensor.video_mode = KAL_TRUE;
	  default: /* ISP_PREVIEW_MODE */
		GC2355MIPI_sensor.video_mode = KAL_FALSE;
	}
	GC2355MIPI_sensor.line_length = GC2355MIPI_PV_PERIOD_PIXEL_NUMS;
	GC2355MIPI_sensor.frame_height = GC2355MIPI_PV_PERIOD_LINE_NUMS;
	GC2355MIPI_sensor.pclk = GC2355MIPI_PREVIEW_CLK;
#if defined(MT6577)||defined(MT6589)
	spin_unlock(&gc2355mipi_drv_lock);
#endif
	//GC2355MIPI_Write_Shutter(GC2355MIPI_sensor.shutter);
	return ERROR_NONE;
}   /*  GC2355MIPIPreview   */

/*************************************************************************
* FUNCTION
*	GC2355MIPICapture
*
* DESCRIPTION
*	This function setup the CMOS sensor in capture MY_OUTPUT mode
*
* PARAMETERS
*
* RETURNS
*	None
*
* GLOBALS AFFECTED
*
*************************************************************************/
UINT32 GC2355MIPICapture(MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *image_window,
						  MSDK_SENSOR_CONFIG_STRUCT *sensor_config_data)
{
	kal_uint32 shutter = (kal_uint32)GC2355MIPI_sensor.shutter;
#if defined(MT6577)||defined(MT6589)
	spin_lock(&gc2355mipi_drv_lock);
#endif
	GC2355MIPI_sensor.video_mode = KAL_FALSE;
		GC2355MIPI_sensor.pv_mode = KAL_FALSE;
#if defined(MT6577)||defined(MT6589)
		spin_unlock(&gc2355mipi_drv_lock);
#endif
	return ERROR_NONE;
}   /* GC2355MIPI_Capture() */

UINT32 GC2355MIPIGetResolution(MSDK_SENSOR_RESOLUTION_INFO_STRUCT *pSensorResolution)
{
	pSensorResolution->SensorFullWidth=GC2355MIPI_IMAGE_SENSOR_FULL_WIDTH;
	pSensorResolution->SensorFullHeight=GC2355MIPI_IMAGE_SENSOR_FULL_HEIGHT;
	pSensorResolution->SensorPreviewWidth=GC2355MIPI_IMAGE_SENSOR_PV_WIDTH;
	pSensorResolution->SensorPreviewHeight=GC2355MIPI_IMAGE_SENSOR_PV_HEIGHT;
	pSensorResolution->SensorVideoWidth=GC2355MIPI_IMAGE_SENSOR_VIDEO_WIDTH;
	pSensorResolution->SensorVideoHeight=GC2355MIPI_IMAGE_SENSOR_VIDEO_HEIGHT;
	return ERROR_NONE;
}	/* GC2355MIPIGetResolution() */

UINT32 GC2355MIPIGetInfo(MSDK_SCENARIO_ID_ENUM ScenarioId,
					  MSDK_SENSOR_INFO_STRUCT *pSensorInfo,
					  MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData)
{
	pSensorInfo->SensorPreviewResolutionX=GC2355MIPI_IMAGE_SENSOR_PV_WIDTH;
	pSensorInfo->SensorPreviewResolutionY=GC2355MIPI_IMAGE_SENSOR_PV_HEIGHT;
	pSensorInfo->SensorFullResolutionX=GC2355MIPI_IMAGE_SENSOR_FULL_WIDTH;
	pSensorInfo->SensorFullResolutionY=GC2355MIPI_IMAGE_SENSOR_FULL_HEIGHT;

	pSensorInfo->SensorCameraPreviewFrameRate=30;
	pSensorInfo->SensorVideoFrameRate=30;
	pSensorInfo->SensorStillCaptureFrameRate=10;
	pSensorInfo->SensorWebCamCaptureFrameRate=15;
	pSensorInfo->SensorResetActiveHigh=TRUE; //low active
	pSensorInfo->SensorResetDelayCount=5; 
	pSensorInfo->SensorOutputDataFormat=GC2355MIPI_COLOR_FORMAT;
	pSensorInfo->SensorClockPolarity=SENSOR_CLOCK_POLARITY_LOW;	
	pSensorInfo->SensorClockFallingPolarity=SENSOR_CLOCK_POLARITY_LOW;
	pSensorInfo->SensorHsyncPolarity = SENSOR_CLOCK_POLARITY_LOW;
	pSensorInfo->SensorVsyncPolarity = SENSOR_CLOCK_POLARITY_LOW;
	

	pSensorInfo->CaptureDelayFrame = 2; 
	pSensorInfo->PreviewDelayFrame = 1;
	pSensorInfo->VideoDelayFrame = 1;

	pSensorInfo->SensorMasterClockSwitch = 0; 
	pSensorInfo->SensorDrivingCurrent = ISP_DRIVING_8MA;
	pSensorInfo->AEShutDelayFrame =0;		   /* The frame of setting shutter default 0 for TG int */
	pSensorInfo->AESensorGainDelayFrame = 0;   /* The frame of setting sensor gain */
	pSensorInfo->AEISPGainDelayFrame = 2;  

	pSensorInfo->SensroInterfaceType=SENSOR_INTERFACE_TYPE_MIPI;
#if defined(GC2355MIPI_1Lane)
	pSensorInfo->SensorMIPILaneNumber = SENSOR_MIPI_1_LANE;
#else
	pSensorInfo->SensorMIPILaneNumber = SENSOR_MIPI_2_LANE;
#endif
	//pSensorInfo->SensorRawType = RAW_TYPE_10BIT;

	pSensorInfo->MIPIDataLowPwr2HighSpeedTermDelayCount = 0; 
	pSensorInfo->MIPIDataLowPwr2HighSpeedSettleDelayCount = 14; 
	pSensorInfo->MIPICLKLowPwr2HighSpeedTermDelayCount = 0;
	pSensorInfo->SensorWidthSampling = 0;
	pSensorInfo->SensorHightSampling = 0;
	pSensorInfo->SensorPacketECCOrder = 1;
	switch (ScenarioId)
	{
		case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
			pSensorInfo->SensorClockFreq=24;
			pSensorInfo->SensorClockDividCount=3;
			pSensorInfo->SensorClockRisingCount= 0;
			pSensorInfo->SensorClockFallingCount= 2;
			pSensorInfo->SensorPixelClockCount= 3;
			pSensorInfo->SensorDataLatchCount= 2;
			pSensorInfo->SensorGrabStartX = GC2355MIPI_PV_X_START; 
			pSensorInfo->SensorGrabStartY = GC2355MIPI_PV_Y_START; 

		break;
		case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
			pSensorInfo->SensorClockFreq=24;
			pSensorInfo->SensorClockDividCount=3;
			pSensorInfo->SensorClockRisingCount= 0;
			pSensorInfo->SensorClockFallingCount= 2;
			pSensorInfo->SensorPixelClockCount= 3;
			pSensorInfo->SensorDataLatchCount= 2;
			pSensorInfo->SensorGrabStartX = GC2355MIPI_VIDEO_X_START; 
			pSensorInfo->SensorGrabStartY = GC2355MIPI_VIDEO_Y_START; 

		break;
		case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
		case MSDK_SCENARIO_ID_CAMERA_ZSD:
			pSensorInfo->SensorClockFreq=24;
			pSensorInfo->SensorClockDividCount= 3;
			pSensorInfo->SensorClockRisingCount=0;
			pSensorInfo->SensorClockFallingCount=2;
			pSensorInfo->SensorPixelClockCount=3;
			pSensorInfo->SensorDataLatchCount=2;
			pSensorInfo->SensorGrabStartX = GC2355MIPI_FULL_X_START; 
			pSensorInfo->SensorGrabStartY = GC2355MIPI_FULL_Y_START; 
		break;
		default:
			pSensorInfo->SensorClockFreq=24;
			pSensorInfo->SensorClockDividCount=3;
			pSensorInfo->SensorClockRisingCount=0;
			pSensorInfo->SensorClockFallingCount=2;		
			pSensorInfo->SensorPixelClockCount=3;
			pSensorInfo->SensorDataLatchCount=2;
			pSensorInfo->SensorGrabStartX = GC2355MIPI_PV_X_START; 
			pSensorInfo->SensorGrabStartY = GC2355MIPI_PV_Y_START; 
		break;
	}
	memcpy(pSensorConfigData, &GC2355MIPI_sensor.cfg_data, sizeof(MSDK_SENSOR_CONFIG_STRUCT));
  return ERROR_NONE;
}	/* GC2355MIPIGetInfo() */


UINT32 GC2355MIPIControl(MSDK_SCENARIO_ID_ENUM ScenarioId, MSDK_SENSOR_EXPOSURE_WINDOW_STRUCT *pImageWindow,
					  MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData)
{

#ifdef GC2355_DRIVER_TRACE
	SENSORDB("GC2355MIPIControl ScenarioId = %d \n",ScenarioId);
#endif
	switch (ScenarioId)
	{
		case MSDK_SCENARIO_ID_CAMERA_PREVIEW:
		case MSDK_SCENARIO_ID_VIDEO_PREVIEW:
			GC2355MIPIPreview(pImageWindow, pSensorConfigData);
		break;
		case MSDK_SCENARIO_ID_CAMERA_CAPTURE_JPEG:
		case MSDK_SCENARIO_ID_CAMERA_ZSD:
			GC2355MIPICapture(pImageWindow, pSensorConfigData);
		break;		
        default:
            return ERROR_INVALID_SCENARIO_ID;
	}
	return ERROR_NONE;
}	/* GC2355MIPIControl() */



UINT32 GC2355MIPISetVideoMode(UINT16 u2FrameRate)
{};

UINT32 GC2235MIPI_SetTestPatternMode(kal_bool bEnable)
{
    kal_uint16 temp;

    SENSORDB("bEnable: %d", bEnable);

   if(bEnable)
        {
                GC2355MIPI_write_cmos_sensor(0xfe, 0x00);
                GC2355MIPI_write_cmos_sensor(0x8b, 0x10);
                GC2355MIPI_write_cmos_sensor(0xb3, 0x40);
                GC2355MIPI_write_cmos_sensor(0xb4, 0x40);
                GC2355MIPI_write_cmos_sensor(0xb5, 0x40);
                GC2355MIPI_write_cmos_sensor(0x03, 0x00);
                GC2355MIPI_write_cmos_sensor(0x04, 0x00);
                //GC2355MIPI_write_cmos_sensor(0x8c, 0x10);
        }
        else
        {
                //GC2355MIPI_write_cmos_sensor(0x8c, 0x00);
      }
 
    return ERROR_NONE;
}

UINT32 GC2355MIPIFeatureControl(MSDK_SENSOR_FEATURE_ENUM FeatureId,
							 UINT8 *pFeaturePara,UINT32 *pFeatureParaLen)
{
	UINT16 *pFeatureReturnPara16=(UINT16 *) pFeaturePara;
	UINT16 *pFeatureData16=(UINT16 *) pFeaturePara;
	UINT32 *pFeatureReturnPara32=(UINT32 *) pFeaturePara;
	//UINT32 *pFeatureData32=(UINT32 *) pFeaturePara;
	//UINT32 GC2355MIPISensorRegNumber;
	//UINT32 i;
	//PNVRAM_SENSOR_DATA_STRUCT pSensorDefaultData=(PNVRAM_SENSOR_DATA_STRUCT) pFeaturePara;
	//MSDK_SENSOR_CONFIG_STRUCT *pSensorConfigData=(MSDK_SENSOR_CONFIG_STRUCT *) pFeaturePara;
	MSDK_SENSOR_REG_INFO_STRUCT *pSensorRegData=(MSDK_SENSOR_REG_INFO_STRUCT *) pFeaturePara;
	//MSDK_SENSOR_GROUP_INFO_STRUCT *pSensorGroupInfo=(MSDK_SENSOR_GROUP_INFO_STRUCT *) pFeaturePara;
	//MSDK_SENSOR_ITEM_INFO_STRUCT *pSensorItemInfo=(MSDK_SENSOR_ITEM_INFO_STRUCT *) pFeaturePara;
	//MSDK_SENSOR_ENG_INFO_STRUCT	*pSensorEngInfo=(MSDK_SENSOR_ENG_INFO_STRUCT *) pFeaturePara;

	switch (FeatureId)
	{
		case SENSOR_FEATURE_GET_RESOLUTION:
			*pFeatureReturnPara16++=GC2355MIPI_IMAGE_SENSOR_FULL_WIDTH;
			*pFeatureReturnPara16=GC2355MIPI_IMAGE_SENSOR_FULL_HEIGHT;
			*pFeatureParaLen=4;
		break;
		case SENSOR_FEATURE_GET_PERIOD:	/* 3 */
			*pFeatureReturnPara16++=GC2355MIPI_sensor.line_length;
			*pFeatureReturnPara16=GC2355MIPI_sensor.frame_height;
			*pFeatureParaLen=4;
		break;
		case SENSOR_FEATURE_GET_PIXEL_CLOCK_FREQ:  /* 3 */
			*pFeatureReturnPara32 = GC2355MIPI_sensor.pclk;
			*pFeatureParaLen=4;
		break;
		case SENSOR_FEATURE_SET_ESHUTTER:	/* 4 */
			GC2355MIPI_set_shutter(*pFeatureData16);
		break;
		case SENSOR_FEATURE_SET_NIGHTMODE:
			GC2355MIPI_night_mode((BOOL) *pFeatureData16);
		break;
		case SENSOR_FEATURE_SET_GAIN:	/* 6 */			
			GC2355MIPI_SetGain((UINT16) *pFeatureData16);
		break;
		case SENSOR_FEATURE_SET_FLASHLIGHT:
		break;
		case SENSOR_FEATURE_SET_ISP_MASTER_CLOCK_FREQ:
		break;
		case SENSOR_FEATURE_SET_REGISTER:
			GC2355MIPI_write_cmos_sensor(pSensorRegData->RegAddr, pSensorRegData->RegData);
		break;
		case SENSOR_FEATURE_GET_REGISTER:
			pSensorRegData->RegData = GC2355MIPI_read_cmos_sensor(pSensorRegData->RegAddr);
		break;
		case SENSOR_FEATURE_SET_CCT_REGISTER:
			memcpy(&GC2355MIPI_sensor.eng.cct, pFeaturePara, sizeof(GC2355MIPI_sensor.eng.cct));
			break;
		break;
		case SENSOR_FEATURE_GET_CCT_REGISTER:	/* 12 */
			if (*pFeatureParaLen >= sizeof(GC2355MIPI_sensor.eng.cct) + sizeof(kal_uint32))
			{
			  *((kal_uint32 *)pFeaturePara++) = sizeof(GC2355MIPI_sensor.eng.cct);
			  memcpy(pFeaturePara, &GC2355MIPI_sensor.eng.cct, sizeof(GC2355MIPI_sensor.eng.cct));
			}
			break;
		case SENSOR_FEATURE_SET_ENG_REGISTER:
			memcpy(&GC2355MIPI_sensor.eng.reg, pFeaturePara, sizeof(GC2355MIPI_sensor.eng.reg));
			break;
		case SENSOR_FEATURE_GET_ENG_REGISTER:	/* 14 */
			if (*pFeatureParaLen >= sizeof(GC2355MIPI_sensor.eng.reg) + sizeof(kal_uint32))
			{
			  *((kal_uint32 *)pFeaturePara++) = sizeof(GC2355MIPI_sensor.eng.reg);
			  memcpy(pFeaturePara, &GC2355MIPI_sensor.eng.reg, sizeof(GC2355MIPI_sensor.eng.reg));
			}
		case SENSOR_FEATURE_GET_REGISTER_DEFAULT:
			((PNVRAM_SENSOR_DATA_STRUCT)pFeaturePara)->Version = NVRAM_CAMERA_SENSOR_FILE_VERSION;
			((PNVRAM_SENSOR_DATA_STRUCT)pFeaturePara)->SensorId = GC2355MIPI_SENSOR_ID;
			memcpy(((PNVRAM_SENSOR_DATA_STRUCT)pFeaturePara)->SensorEngReg, &GC2355MIPI_sensor.eng.reg, sizeof(GC2355MIPI_sensor.eng.reg));
			memcpy(((PNVRAM_SENSOR_DATA_STRUCT)pFeaturePara)->SensorCCTReg, &GC2355MIPI_sensor.eng.cct, sizeof(GC2355MIPI_sensor.eng.cct));
			*pFeatureParaLen = sizeof(NVRAM_SENSOR_DATA_STRUCT);
			break;
		case SENSOR_FEATURE_GET_CONFIG_PARA:
			memcpy(pFeaturePara, &GC2355MIPI_sensor.cfg_data, sizeof(GC2355MIPI_sensor.cfg_data));
			*pFeatureParaLen = sizeof(GC2355MIPI_sensor.cfg_data);
			break;
		case SENSOR_FEATURE_CAMERA_PARA_TO_SENSOR:
		     GC2355MIPI_camera_para_to_sensor();
			break;
		case SENSOR_FEATURE_SENSOR_TO_CAMERA_PARA:
			GC2355MIPI_sensor_to_camera_para();
			break;							
		case SENSOR_FEATURE_GET_GROUP_COUNT:
			GC2355MIPI_get_sensor_group_count((kal_uint32 *)pFeaturePara);
			*pFeatureParaLen = 4;
			break;
		case SENSOR_FEATURE_GET_GROUP_INFO:
			GC2355MIPI_get_sensor_group_info((MSDK_SENSOR_GROUP_INFO_STRUCT *)pFeaturePara);
			*pFeatureParaLen = sizeof(MSDK_SENSOR_GROUP_INFO_STRUCT);
			break;
		case SENSOR_FEATURE_GET_ITEM_INFO:
			GC2355MIPI_get_sensor_item_info((MSDK_SENSOR_ITEM_INFO_STRUCT *)pFeaturePara);
			*pFeatureParaLen = sizeof(MSDK_SENSOR_ITEM_INFO_STRUCT);
			break;
		case SENSOR_FEATURE_SET_ITEM_INFO:
			GC2355MIPI_set_sensor_item_info((MSDK_SENSOR_ITEM_INFO_STRUCT *)pFeaturePara);
			*pFeatureParaLen = sizeof(MSDK_SENSOR_ITEM_INFO_STRUCT);
			break;
		case SENSOR_FEATURE_GET_ENG_INFO:
			memcpy(pFeaturePara, &GC2355MIPI_sensor.eng_info, sizeof(GC2355MIPI_sensor.eng_info));
			*pFeatureParaLen = sizeof(GC2355MIPI_sensor.eng_info);
			break;
		case SENSOR_FEATURE_GET_LENS_DRIVER_ID:
			// get the lens driver ID from EEPROM or just return LENS_DRIVER_ID_DO_NOT_CARE
			// if EEPROM does not exist in camera module.
			*pFeatureReturnPara32=LENS_DRIVER_ID_DO_NOT_CARE;
			*pFeatureParaLen=4;
		break;
		case SENSOR_FEATURE_SET_VIDEO_MODE:
		   //GC2355MIPISetVideoMode(*pFeatureData16);
			break; 
		case SENSOR_FEATURE_CHECK_SENSOR_ID:
			GC2355MIPIGetSensorID(pFeatureReturnPara32); 
			break; 
		case SENSOR_FEATURE_SET_AUTO_FLICKER_MODE:
			break;
		case SENSOR_FEATURE_SET_TEST_PATTERN:
        		GC2235MIPI_SetTestPatternMode((BOOL)*pFeatureData16);
        		break;
    		case SENSOR_FEATURE_GET_TEST_PATTERN_CHECKSUM_VALUE://for factory mode auto testing             
        		*pFeatureReturnPara32= GC2235MIPI_TEST_PATTERN_CHECKSUM;
        		*pFeatureParaLen=4;                             
         		break;
		default:
			break;
	}
	return ERROR_NONE;
}	/* GC2355MIPIFeatureControl() */
SENSOR_FUNCTION_STRUCT	SensorFuncGC2355MIPI=
{
	GC2355MIPIOpen,
	GC2355MIPIGetInfo,
	GC2355MIPIGetResolution,
	GC2355MIPIFeatureControl,
	GC2355MIPIControl,
	GC2355MIPIClose
};

UINT32 GC2355MIPI_RAW_SensorInit(PSENSOR_FUNCTION_STRUCT *pfFunc) 
{
	/* To Do : Check Sensor status here */
	if (pfFunc!=NULL)
		*pfFunc=&SensorFuncGC2355MIPI;

	return ERROR_NONE;
}	/* SensorInit() */
