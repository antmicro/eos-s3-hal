/*
 * ==========================================================
 *
 *    Copyright (C) 2020 QuickLogic Corporation             
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 * 		http://www.apache.org/licenses/LICENSE-2.0
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 *
 *    File      : eoss3_hal_fpga_adc.c
 *    Purpose :  
 *                                                          
 * ===========================================================
 *
 */
#include "Fw_global_config.h"

#include <stdio.h>

#include "eoss3_hal_fpga_sdma_api.h"
#include "eoss3_hal_fpga_adc_reg.h"
//#include "eoss3_hal_gpio.h"
#include "eoss3_hal_fpga_adc_api.h"

#include "s3x_clock.h"
#include "eoss3_hal_def.h"
#include "dbg_uart.h"


typedef struct
{
  void *sdma_handle;
  void (*callback_rxptr)(void);
} adc_info_t;

static const struct LTC1859_ymx_plus_b convert_table[] = {

    /* there are really only 4 combinations we care about, bits [3:2] */
    /* the mask value for those 2 bits are 0x0c */
    { .masked_config_value = 0x00, 
      .slopeM = 5000000, /* +/-5v range */
      .slopeD = 32767, 
      .intercept = 0, 
      .python_format_char = 'h',
      .is_signed = 1,
      .dummy_pad = 0,
    },
    
    { .masked_config_value = 0x04, 
      .slopeM = 10000000, /* +/-10v range */
      .slopeD = 32767,
      .intercept = 0, 
      .python_format_char = 'h',
      .is_signed = 1,
      .dummy_pad = 0,
    },
   
    { .masked_config_value = 0x08, 
      .slopeM = 5000000,
      .slopeD = 65535, /* 0-5v range */
      .intercept = 0, 
      .python_format_char = 'H',
      .is_signed = 0,
      .dummy_pad = 0,
    },
    
    { .masked_config_value = 0x0c, 
      .slopeM = 10000000,
      .slopeD = 65535,  /* 0-10v range */
      .intercept = 0, 
      .python_format_char = 'H',
      .is_signed = 0,
      .dummy_pad = 0,
    },
    
    /* this is the LAST element
     * it is also the termination element
     * it is used when something is wrong
     * or the channel is disabled.
     */
    { .masked_config_value = 1, 
      .slopeM = 0,
      .slopeD = 1,
      .intercept = 0, 
      .python_format_char = 'H',
      .is_signed = 0,
      .dummy_pad = 0,
    }
};

const struct LTC1859_ymx_plus_b *HAL_LTC1859_CfgToYmxB( int config )
{
    const struct LTC1859_ymx_plus_b *p;
    
    
    /* if channel is disabled */
    if( config & 1 ){
        /* use 1 entry in table */;
        config = 1;
    } else {
        /* keep only the bits we care about */
        config = config & 0x0c;
    }
    

    p = convert_table;
    while(p->slopeM){
        if( p->masked_config_value == config ){
            return p;
        }
        p++;
    }
    dbg_fatal_error_hex32("ltc1859-unknown-conversion", config );
    /* make compiler happy, return something */
    return NULL;
}


/* given a config byte value - convert ADC counts to micro-volts */
int LTC1859_to_uVolts( uint8_t cfg, uint16_t value )
{
    const struct LTC1859_ymx_plus_b *pMXPB;
    int64_t ivalue;
    int64_t ovalue;

    /* see data sheet for magic number explination */
    if( (cfg&1)==1 ){
        return 0;
    }
    
    pMXPB = HAL_LTC1859_CfgToYmxB( cfg );
    
    /* sign extend? */
    if(pMXPB->is_signed){
        ivalue = (int16_t)(value);
    } else {
        ivalue = (uint16_t)(value);
    }
    
    ovalue = (ivalue * pMXPB->slopeM);
    ovalue = ovalue / pMXPB->slopeD;
    ovalue = ovalue + pMXPB->intercept;
    
    return ovalue;
}


static  adc_info_t adc_info_state;  //To store state info

void sdma_callback(void *handle)
{
  //call back to user for dma readcomplete
  (*adc_info_state.callback_rxptr)();
  return;
}


HAL_StatusTypeDef HAL_LTC1859_ADC_Init( HAL_LTC1859_cfg_t *adc_cfg, void (*pcb_read)(void))
{
  ch_cfg_t cfg;
  cfg.data_size = DATASIZE_WORD;
  cfg.r_power   = 0;

  if (NULL == pcb_read)
  {
        //dbg_str("Read call back is NULL:\n");
    return HAL_ERROR;
  }

  adc_info_state.callback_rxptr = pcb_read;

  //Enable FB clocks.
  S3x_Clk_Set_Rate(S3X_FB_16_CLK, HSOSC_18MHZ);
  S3x_Clk_Enable(S3X_FB_16_CLK);

  S3x_Clk_Set_Rate(S3X_FB_21_CLK, HSOSC_4MHZ);
  S3x_Clk_Enable(S3X_FB_21_CLK);


#if (CONST_FREQ == 0)
  S3x_Register_Qos_Node(S3X_FB_16_CLK);
  S3x_Set_Qos_Req(S3X_FB_16_CLK, MIN_HSOSC_FREQ, HSOSC_72MHZ);
#endif

  //Read Chip-ID register
  if (ADC_CHIP_ID_VAL != FPGA_ADC->CHIP_ID)
  {
        dbg_fatal_error("bad-ltc1859-fpga\n");
    return HAL_ERROR;
  }

  HAL_FSDMA_Init();
  //Get dma channel HAL_FSDMA_GetChannel
  adc_info_state.sdma_handle = HAL_FSDMA_GetChannel(&sdma_callback, &cfg);
  if (NULL == adc_info_state.sdma_handle)
  {
        dbg_fatal_error("ltc1859-dma-null\n");
    return HAL_ERROR;
  }
    if( DBG_flags & DBG_FLAG_adc_task ){
        dbg_str_ptr("ltc1859-dma", adc_info_state.sdma_handle );
    }


    if ((adc_cfg->channel_enable_bits > 0xF) || (adc_cfg->channel_enable_bits == 0x0))
  {
        dbg_fatal_error("bad-ltc1859-emable\n");
    return HAL_ERROR;
  }
    if (adc_cfg->channel_enable_bits & CHANNEL0_ENABLE_MASK)
  {
        FPGA_ADC->SEN1_SETTING = adc_cfg->chnl_commands[0];
  }
    if (adc_cfg->channel_enable_bits & CHANNEL1_ENABLE_MASK)
  {
        FPGA_ADC->SEN2_SETTING = adc_cfg->chnl_commands[1];
  }
    if (adc_cfg->channel_enable_bits & CHANNEL2_ENABLE_MASK)
  {
        FPGA_ADC->SEN3_SETTING = adc_cfg->chnl_commands[2];
  }
    if (adc_cfg->channel_enable_bits & CHANNEL3_ENABLE_MASK)
  {
        FPGA_ADC->SEN4_SETTING = adc_cfg->chnl_commands[3];
  }
    FPGA_ADC->SEN_ENR = adc_cfg->channel_enable_bits;

#if 0
  if (SENSOR_SAMPLE_RATE_100KHZ == adc_cfg->frequency)
  {
    FPGA_ADC->TIMER_COUNT = SENSOR_SAMPLE_RATE_100KHZ_DIV_VALUE;
  }
  else if (SENSOR_SAMPLE_RATE_16KHZ == adc_cfg->frequency)
  {
    FPGA_ADC->TIMER_COUNT = SENSOR_SAMPLE_RATE_16KHZ_DIV_VALUE;
  }
  else
  {
    printf("Sensor Rate configuration Error\n");
    return HAL_ERROR;
  }
#else
    uint32_t tmp;
    
    tmp = (FOUR_MEGA_HZ + (adc_cfg->frequency/2)) / adc_cfg->frequency;
    FPGA_ADC->TIMER_COUNT = tmp;
#endif

  //Enable Timer postpone after dma settings.
  //FPGA_ADC->TIMER_ENABLE = ADC_TIMER_ENABLE_BIT;

  /* flush the fifo at init time */
  FPGA_ADC->FIFO_RESET = 1;
  return HAL_OK;
}


HAL_StatusTypeDef HAL_LTC1859_ADC_Read(void *buffer, size_t n_bytes)
{
  HAL_StatusTypeDef dmaStatus;

  if (NULL == buffer)
  {
    return HAL_ERROR;
  }

    if( n_bytes & 3 ){
        return HAL_ERROR;
    }
    if( adc_info_state.sdma_handle == NULL ){
        dbg_fatal_error("ltc1859-rd-no-dma\n");
    }

  dmaStatus = HAL_FSDMA_Receive(adc_info_state.sdma_handle, (void *)buffer, n_bytes);
    if (HAL_OK != dmaStatus)
  {
        //printf("dma read failed");
        return dmaStatus;
  }

  //Enable Timer
  FPGA_ADC->TIMER_ENABLE = ADC_TIMER_ENABLE_BIT;


  return HAL_OK;
}

void HAL_LTC1859_ADC_De_Init(void)
{
  HAL_StatusTypeDef dmaStatus;
    if( adc_info_state.sdma_handle == NULL ){
        dbg_fatal_error("ltc1859-close-error\n");
    }
    
    if( DBG_flags & DBG_FLAG_adc_task ){
        dbg_str_ptr("ltc1859-dma-close", adc_info_state.sdma_handle );
    }
  dmaStatus = HAL_FSDMA_ReleaseChannel(adc_info_state.sdma_handle);
    if (HAL_OK != dmaStatus){
        dbg_fatal_error("Error in Releasing SDMA channel");
    }
    adc_info_state.sdma_handle = NULL;
  FPGA_ADC->TIMER_ENABLE = 0;  //Disable TIMER to stop sensor data capture.

#if (CONST_FREQ == 0)
     S3x_Clear_Qos_Req(S3X_FB_16_CLK, MIN_HSOSC_FREQ);
#endif

  S3x_Clk_Disable(S3X_FB_21_CLK);
  S3x_Clk_Disable(S3X_FB_16_CLK);

  return;
}
