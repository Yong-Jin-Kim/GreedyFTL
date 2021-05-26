//////////////////////////////////////////////////////////////////////////////////
// nvme_main.c for Cosmos+ OpenSSD
// Copyright (c) 2016 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Youngjin Jo <yjjo@enc.hanyang.ac.kr>
//				  Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//				  Kibin Park <kbpark@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//			 Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//			 Kibin Park <kbpark@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: NVMe Main
// File Name: nvme_main.c
//
// Version: v1.2.0
//
// Description:
//   - initializes FTL and NAND
//   - handles NVMe controller
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.2.0
//   - header file for buffer is changed from "ia_lru_buffer.h" to "lru_buffer.h"
//   - Low level scheduler execution is allowed when there is no i/o command
//
// * v1.1.0
//   - DMA status initialization is added
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////

#include "xil_printf.h"
#include "debug.h"
#include "io_access.h"
#include "xtime_l.h"

#include "nvme.h"
#include "host_lld.h"
#include "nvme_main.h"
#include "nvme_admin_cmd.h"
#include "nvme_io_cmd.h"

#include "../memory_map.h"



volatile NVME_CONTEXT g_nvmeTask;

BARRIER_CONTEXT g_barrierContext;

void barrier_init()
{
	BARRIER_STREAM_LIST* p_stream = &g_barrierContext.stream[0];

	p_stream->head_idx = 0;
	p_stream->tail_idx = 0;

	p_stream->count = 0;

	for (unsigned int idx = 0; idx < 256; idx++)
	{
		p_stream->epoch_list[idx] = INVALID_EPOCH_ID;
	}

	p_stream = &g_barrierContext.stream[1];

	p_stream->head_idx = 0;
	p_stream->tail_idx = 0;

	p_stream->count = 0;

	for (unsigned int idx = 0; idx < 256; idx++)
	{
		p_stream->epoch_list[idx] = INVALID_EPOCH_ID;
	}
}

void barrier_push_epoch(unsigned int stream_id, unsigned int epoch_id)
{
	ASSERT(stream_id > 0 && stream_id <= 2);

	BARRIER_STREAM_LIST* p_stream = &g_barrierContext.stream[stream_id-1];

	if (p_stream->count > 256)
	{
		// SP: handling list full case.
		ASSERT(FALSE);
	}

	p_stream->epoch_list[p_stream->tail_idx] = epoch_id;

	p_stream->tail_idx = (p_stream->tail_idx+1)%256;
	p_stream->count++;
}

unsigned int barrier_pop_epoch(unsigned int stream_id)
{
	ASSERT(stream_id > 0 && stream_id <= 2);

	BARRIER_STREAM_LIST* p_stream = &g_barrierContext.stream[stream_id-1];

	ASSERT(p_stream->count > 0);

	unsigned int epoch_id = p_stream->epoch_list[p_stream->head_idx];

	p_stream->head_idx = (p_stream->head_idx+1)%256;
	p_stream->count--;

	return epoch_id;
}

unsigned int barrier_get_epoch_count(unsigned int stream_id)
{
	ASSERT(stream_id > 0 && stream_id <= 2);

	BARRIER_STREAM_LIST* p_stream = &g_barrierContext.stream[stream_id-1];

	return p_stream->count;
}

void nvme_main()
{
	unsigned int exeLlr;
	unsigned int rstCnt = 0;
	XTime timeTickStart, timeTickEnd;

	xil_printf("!!! Wait until FTL reset complete !!! \r\n");

	InitFTL();

#if (SUPPORT_BARRIER_FTL == 1)
	barrier_init();
#endif

	xil_printf("\r\nFTL reset complete!!! \r\n");
	xil_printf("Turn on the host PC \r\n");

	while(1)
	{
		exeLlr = 1;

		if(g_nvmeTask.status == NVME_TASK_RUNNING)
		{
			NVME_COMMAND nvmeCmd;
			unsigned int cmdValid;
			cmdValid = get_nvme_cmd(&nvmeCmd.qID, &nvmeCmd.cmdSlotTag, &nvmeCmd.cmdSeqNum, nvmeCmd.cmdDword);
			if(cmdValid == 1)
			{	rstCnt = 0;
				if(nvmeCmd.qID == 0)
				{
					handle_nvme_admin_cmd(&nvmeCmd);
				}
				else
				{
					handle_nvme_io_cmd(&nvmeCmd);
					ReqTransSliceToLowLevel();
					exeLlr=0;
				}
			}

			// SP: update current tick
			XTime_GetTime(&timeTickEnd);
#if 0
			if (INTERNAL_FLUSH_PERIOD_MS == GET_TIME_MS(timeTickStart, timeTickEnd))
			{
#if (SUPPORT_BARRIER_FTL == 0)
				FlushWriteDataToNand();
#else
				// SP: flush write data with barrier flag.

				unsigned int numFlushEpochCount = barrier_get_epoch_count(1);
				unsigned int curEpochId;

				while (0 < numFlushEpochCount)
				{
					curEpochId = barrier_pop_epoch(1);
					xil_printf("\r\n[IF] str1, epoch:%u\r\n", curEpochId);
					FlushWriteDataToNand2(1, curEpochId);
				}

				numFlushEpochCount = barrier_get_epoch_count(2);

				while (0 < numFlushEpochCount)
				{
					curEpochId = barrier_pop_epoch(2);
					xil_printf("\r\n[IF] str2, epoch:%u\r\n", curEpochId);
					FlushWriteDataToNand2(2, curEpochId);
				}
#endif

				// SP: update start tick
				XTime_GetTime(&timeTickStart);
			}
#endif
		}
		else if(g_nvmeTask.status == NVME_TASK_WAIT_CC_EN)
		{
			unsigned int ccEn;
			ccEn = check_nvme_cc_en();
			if(ccEn == 1)
			{
				set_nvme_admin_queue(1, 1, 1);
				set_nvme_csts_rdy(1);
				g_nvmeTask.status = NVME_TASK_RUNNING;
				xil_printf("\r\nNVMe ready!!!\r\n");

				// SP: get timeTickStart for internal flush
				XTime_GetTime(&timeTickStart);
			}
		}
		else if(g_nvmeTask.status == NVME_TASK_SHUTDOWN)
		{
			NVME_STATUS_REG nvmeReg;
			nvmeReg.dword = IO_READ32(NVME_STATUS_REG_ADDR);
			if(nvmeReg.ccShn != 0)
			{
				unsigned int qID;
				set_nvme_csts_shst(1);

				for(qID = 0; qID < 8; qID++)
				{
					set_io_cq(qID, 0, 0, 0, 0, 0, 0);
					set_io_sq(qID, 0, 0, 0, 0, 0);
				}

				set_nvme_admin_queue(0, 0, 0);
				g_nvmeTask.cacheEn = 0;
				set_nvme_csts_shst(2);
				g_nvmeTask.status = NVME_TASK_WAIT_RESET;

				//flush grown bad block info
				UpdateBadBlockTableForGrownBadBlock(RESERVED_DATA_BUFFER_BASE_ADDR);

				xil_printf("\r\nNVMe shutdown!!!\r\n");
			}
		}
		else if(g_nvmeTask.status == NVME_TASK_WAIT_RESET)
		{
			unsigned int ccEn;
			ccEn = check_nvme_cc_en();
			if(ccEn == 0)
			{
				g_nvmeTask.cacheEn = 0;
				set_nvme_csts_shst(0);
				set_nvme_csts_rdy(0);
				g_nvmeTask.status = NVME_TASK_IDLE;
				xil_printf("\r\nNVMe disable!!!\r\n");
			}
		}
		else if(g_nvmeTask.status == NVME_TASK_RESET)
		{
			unsigned int qID;
			for(qID = 0; qID < 8; qID++)
			{
				set_io_cq(qID, 0, 0, 0, 0, 0, 0);
				set_io_sq(qID, 0, 0, 0, 0, 0);
			}

			if (rstCnt>= 5){
				pcie_async_reset(rstCnt);
				rstCnt = 0;
				xil_printf("\r\nPcie iink disable!!!\r\n");
				xil_printf("Wait few minute or reconnect the PCIe cable\r\n");
			}
			else
				rstCnt++;

			g_nvmeTask.cacheEn = 0;
			set_nvme_admin_queue(0, 0, 0);
			set_nvme_csts_shst(0);
			set_nvme_csts_rdy(0);
			g_nvmeTask.status = NVME_TASK_IDLE;

			xil_printf("\r\nNVMe reset!!!\r\n");
		}

		if(exeLlr && ((nvmeDmaReqQ.headReq != REQ_SLOT_TAG_NONE) || notCompletedNandReqCnt || blockedReqCnt))
		{
			CheckDoneNvmeDmaReq();
			SchedulingNandReq();
		}
	}
}


