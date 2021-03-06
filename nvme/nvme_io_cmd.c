//////////////////////////////////////////////////////////////////////////////////
// nvme_io_cmd.c for Cosmos+ OpenSSD
// Copyright (c) 2016 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Youngjin Jo <yjjo@enc.hanyang.ac.kr>
//				  Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
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
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: NVMe IO Command Handler
// File Name: nvme_io_cmd.c
//
// Version: v1.0.1
//
// Description:
//   - handles NVMe IO command
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.1
//   - header file for buffer is changed from "ia_lru_buffer.h" to "lru_buffer.h"
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////


#include "xil_printf.h"
#include "debug.h"
#include "io_access.h"

#include "nvme.h"
#include "host_lld.h"
#include "nvme_io_cmd.h"
#include "nvme_main.h"

#include "../ftl_config.h"
#include "../request_transform.h"

#include "xil_exception.h"
#include "../request_schedule.h"
#include "../request_transform.h"

#include "../data_buffer.h"
#include "../request_allocation.h"
#include "../address_translation.h"

static void _handle_nvme_io_read(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd)
{
	IO_READ_COMMAND_DW12 readInfo12;
	//IO_READ_COMMAND_DW13 readInfo13;
	//IO_READ_COMMAND_DW15 readInfo15;
	unsigned int startLba[2];
	unsigned int nlb;

	readInfo12.dword = nvmeIOCmd->dword[12];
	//readInfo13.dword = nvmeIOCmd->dword[13];
	//readInfo15.dword = nvmeIOCmd->dword[15];

	startLba[0] = nvmeIOCmd->dword[10];
	startLba[1] = nvmeIOCmd->dword[11];
	nlb = readInfo12.NLB;

	ASSERT(startLba[0] < storageCapacity_L && (startLba[1] < STORAGE_CAPACITY_H || startLba[1] == 0));
	//ASSERT(nlb < MAX_NUM_OF_NLB);
	ASSERT((nvmeIOCmd->PRP1[0] & 0x3) == 0 && (nvmeIOCmd->PRP2[0] & 0x3) == 0); //error
	ASSERT(nvmeIOCmd->PRP1[1] < 0x10000 && nvmeIOCmd->PRP2[1] < 0x10000);

	ReqTransNvmeToSlice(cmdSlotTag, startLba[0], nlb, IO_NVM_READ);
}


static void _handle_nvme_io_write(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd)
{
	IO_WRITE_COMMAND_DW12 writeInfo12;
	//IO_WRITE_COMMAND_DW13 writeInfo13;
	//IO_WRITE_COMMAND_DW15 writeInfo15;
	unsigned int startLba[2];
	unsigned int nlb;

	writeInfo12.dword = nvmeIOCmd->dword[12];
	//writeInfo13.dword = nvmeIOCmd->dword[13];
	//writeInfo15.dword = nvmeIOCmd->dword[15];

	//if(writeInfo12.FUA == 1)
	//	xil_printf("write FUA\r\n");

#if (SUPPORT_BARRIER_FTL == 1)
	if (TRUE == writeInfo12.barrier_flag1)
	{
		// SP: need to flush current epoch data of stream 1 after dma of this command
		// nvmeIOCmd->stream_id1, nvmeIOCmd->epoch_id1
	}
	if (TRUE == writeInfo12.barrier_flag2)
	{
		// SP: need to flush current epoch data of stream 2 after dma of this command
		// nvmeIOCmd->stream_id2, nvmeIOCmd->epoch_id2
	}
#endif //#if (SUPPORT_BARRIER_FTL == 1)

	startLba[0] = nvmeIOCmd->dword[10];
	startLba[1] = nvmeIOCmd->dword[11];
	nlb = writeInfo12.NLB;

	ASSERT(startLba[0] < storageCapacity_L && (startLba[1] < STORAGE_CAPACITY_H || startLba[1] == 0));
	//ASSERT(nlb < MAX_NUM_OF_NLB);
	ASSERT((nvmeIOCmd->PRP1[0] & 0xF) == 0 && (nvmeIOCmd->PRP2[0] & 0xF) == 0);
	ASSERT(nvmeIOCmd->PRP1[1] < 0x10000 && nvmeIOCmd->PRP2[1] < 0x10000);

#if (SUPPORT_BARRIER_FTL == 1)
	// TODO:
	// adding parameter stream_ids, epoch_ids into request slice
	// adding parameter stream_ids, epoch_ids into data buffer entry
	ReqTransNvmeToSliceForWrite(cmdSlotTag, nvmeIOCmd);
#else
	ReqTransNvmeToSlice(cmdSlotTag, startLba[0], nlb, IO_NVM_WRITE);
#endif
}

static void _handle_nvme_io_flush(unsigned int cmdSlotTag)
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
}

void handle_nvme_io_cmd(NVME_COMMAND *nvmeCmd)
{
	NVME_IO_COMMAND *nvmeIOCmd = (NVME_IO_COMMAND*)nvmeCmd;

	/*
	xil_printf("OPC = 0x%X\r\n", nvmeIOCmd->OPC);
	xil_printf("PRP1[63:32] = 0x%X, PRP1[31:0] = 0x%X\r\n", nvmeIOCmd->PRP1[1], nvmeIOCmd->PRP1[0]);
	xil_printf("PRP2[63:32] = 0x%X, PRP2[31:0] = 0x%X\r\n", nvmeIOCmd->PRP2[1], nvmeIOCmd->PRP2[0]);
	xil_printf("dword10 = 0x%X\r\n", nvmeIOCmd->dword10);
	xil_printf("dword11 = 0x%X\r\n", nvmeIOCmd->dword11);
	xil_printf("dword12 = 0x%X\r\n", nvmeIOCmd->dword12);
	*/

	switch(nvmeIOCmd->OPC)
	{
		case IO_NVM_FLUSH:
		{
			xil_printf("IO Flush Command\r\n");

			_handle_nvme_io_flush(nvmeCmd->cmdSlotTag);

			NVME_COMPLETION nvmeCPL;
			nvmeCPL.dword[0] = 0;
			nvmeCPL.specific = 0x0;

			set_auto_nvme_cpl(nvmeCmd->cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
			break;
		}
		case IO_NVM_WRITE:
		{
			//xil_printf("IO Write Command\r\n");
			_handle_nvme_io_write(nvmeCmd->cmdSlotTag, nvmeIOCmd);
			break;
		}
		case IO_NVM_READ:
		{
			//xil_printf("IO Read Command\r\n");
			_handle_nvme_io_read(nvmeCmd->cmdSlotTag, nvmeIOCmd);
			break;
		}
		default:
		{
			xil_printf("Not Support IO Command OPC: %X\r\n", nvmeIOCmd->OPC);
			ASSERT(0);
			break;
		}
	}
}

