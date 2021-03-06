/* C:B**************************************************************************
This software is Copyright 2016 Alexander Motin <mav@FreeBSD.org>
This software is Copyright 2017 Spectra Logic Corporation

This file is part of sedutil.

sedutil is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

sedutil is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with sedutil.  If not, see <http://www.gnu.org/licenses/>.

 * C:E********************************************************************** */
#include "os.h"
#include <sys/types.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fstream>
#include <camlib.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>
#include "DtaDevFreeBSDSata.h"
#include "DtaHexDump.h"
#include "DtaLexicon.h"

using namespace std;

/** The Device class represents a single disk device.
 *  FreeBSD specific implementation using the CAM pass interface
 */

DtaDevFreeBSDSata::DtaDevFreeBSDSata()
{
	dev_protocol = PROTO_UNSPECIFIED;
}

bool DtaDevFreeBSDSata::init(const char * devref)
{
	LOG(D1) << "Creating DtaDevFreeBSDSata::DtaDev() " << devref;

	if ((camdev = cam_open_device(devref, O_RDWR)) == NULL) {
		// This is a D1 because diskscan looks for open fail to end scan
		LOG(D1) << "Error opening device " << devref;
		return (FALSE);
	}
	return (TRUE);
}

/** Send an ioctl to the device using pass through. */
uint8_t DtaDevFreeBSDSata::sendCmd(ATACOMMAND cmd, uint8_t protocol, uint16_t comID,
                         void * buffer, uint32_t bufferlen)
{
	union ccb ccb;

	bzero(&ccb, sizeof(ccb));
	switch (dev_protocol) {
	case PROTO_SCSI:
		cam_fill_csio(&ccb.csio, 1, NULL,
		    (cmd == IF_RECV) ? CAM_DIR_IN : CAM_DIR_OUT,
		    MSG_SIMPLE_Q_TAG, (u_int8_t*)buffer, bufferlen,
		    SSD_FULL_SIZE, 12, 60 * 1000);

		ccb.csio.cdb_io.cdb_bytes[0] = (cmd == IF_RECV) ? 0xa2 : 0xb5;
		ccb.csio.cdb_io.cdb_bytes[1] = protocol;
		ccb.csio.cdb_io.cdb_bytes[2] = comID >> 8;
		ccb.csio.cdb_io.cdb_bytes[3] = comID;
		ccb.csio.cdb_io.cdb_bytes[4] = 0x80;
		ccb.csio.cdb_io.cdb_bytes[6] = (bufferlen/512) >> 24;
		ccb.csio.cdb_io.cdb_bytes[7] = (bufferlen/512) >> 16;
		ccb.csio.cdb_io.cdb_bytes[8] = (bufferlen/512) >> 8;
		ccb.csio.cdb_io.cdb_bytes[9] = (bufferlen/512);
		break;
	case PROTO_ATA:
		cam_fill_ataio(&ccb.ataio, 0, NULL,
		    (cmd == IF_RECV) ? CAM_DIR_IN : CAM_DIR_OUT,
		    MSG_SIMPLE_Q_TAG, (u_int8_t*)buffer, bufferlen, 60 * 1000);

		ccb.ataio.cmd.flags = 0;
		ccb.ataio.cmd.command = cmd;
		ccb.ataio.cmd.features = protocol;
		ccb.ataio.cmd.lba_low = (bufferlen / 512) >> 8;
		ccb.ataio.cmd.lba_mid = (comID & 0x00ff);
		ccb.ataio.cmd.lba_high = (comID & 0xff00) >> 8;
		ccb.ataio.cmd.device = 0x40;
		ccb.ataio.cmd.sector_count = bufferlen / 512;
		break;
#if (__FreeBSD_version >= 1200039)
	/*
	 * Note that the version here isn't presise.  NVMe support was
	 * added to the pass(4) driver on 7/14/2017, and this particular
	 * version is from 7/22/2017, when clang 5.0 was imported.
	 */
	case PROTO_NVME:
		cam_fill_nvmeadmin(&ccb.nvmeio, 0, NULL,
		    (cmd == IF_RECV) ?  CAM_DIR_IN : CAM_DIR_OUT,
		    (uint8_t *)buffer, bufferlen, 60 * 1000);
		if (cmd == IF_RECV)
			ccb.nvmeio.cmd.opc = NVME_OPC_SECURITY_RECEIVE;
		else
			ccb.nvmeio.cmd.opc = NVME_OPC_SECURITY_SEND;
		ccb.nvmeio.cmd.cdw10 = protocol << 24 | comID << 8;
		ccb.nvmeio.cmd.cdw11 = bufferlen;
		break;
#endif
	default:
		LOG(E) << "Unknown drive protocol" << dev_protocol;
		return (FAIL);
		break; /*NOTREACHED*/
	}

	ccb.ccb_h.flags |= CAM_DEV_QFRZDIS;

	if (cam_send_ccb(camdev, &ccb) < 0) {
		LOG(D4) << "cam_send_ccb failed";
		return (FAIL);
	}

	if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
		IFLOG(D4)
			cam_error_print(camdev, &ccb, CAM_ESF_ALL,
			    CAM_EPF_ALL, stderr);
		return (FAIL);
	}
	return (0);
}

static void safecopy(uint8_t * dst, size_t dstsize, uint8_t * src, size_t srcsize)
{
	const size_t size = min(dstsize, srcsize);
	if (size > 0) memcpy(dst, src, size);
	if (size < dstsize) memset(dst+size, '\0', dstsize-size);
}

void DtaDevFreeBSDSata::identify(OPAL_DiskInfo& disk_info)
{
	union ccb ccb;

	LOG(D4) << "Entering DtaDevFreeBSDSata::identify()";

	bzero(&ccb, sizeof(union ccb));
	ccb.ccb_h.func_code = XPT_GDEV_TYPE;
	if (cam_send_ccb(camdev, &ccb) < 0) {
		LOG(D4) << "cam_send_ccb failed";
		disk_info.devType = DEVICE_TYPE_OTHER;
		return;
	}

	if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
		LOG(D4) << "cam_send_ccb error" << ccb.ccb_h.status;
		disk_info.devType = DEVICE_TYPE_OTHER;
		return;
	}

	dev_protocol = ccb.cgd.protocol;

	switch (dev_protocol) {
	case PROTO_SCSI:
		disk_info.devType = DEVICE_TYPE_SAS;
		safecopy(disk_info.serialNum, sizeof(disk_info.serialNum),
		    (uint8_t *)ccb.cgd.serial_num, ccb.cgd.serial_num_len);
		safecopy(disk_info.firmwareRev, sizeof(disk_info.firmwareRev),
		    (uint8_t *)ccb.cgd.inq_data.revision,
		    sizeof(ccb.cgd.inq_data.revision));
		safecopy(disk_info.modelNum, sizeof(disk_info.modelNum),
		    (uint8_t *)ccb.cgd.inq_data.vendor,
		    sizeof(ccb.cgd.inq_data.vendor) +
		    sizeof(ccb.cgd.inq_data.product));
		break;
	case PROTO_ATA:
		disk_info.devType = DEVICE_TYPE_ATA;
		safecopy(disk_info.serialNum, sizeof(disk_info.serialNum),
		    (uint8_t *)ccb.cgd.serial_num, ccb.cgd.serial_num_len);
		safecopy(disk_info.firmwareRev, sizeof(disk_info.firmwareRev),
		    ccb.cgd.ident_data.revision,
		    sizeof(ccb.cgd.ident_data.revision));
		safecopy(disk_info.modelNum, sizeof(disk_info.modelNum),
		    ccb.cgd.ident_data.model, sizeof(ccb.cgd.ident_data.model));
		break;
#if (__FreeBSD_version >= 1200039)
	/*
	 * Note that the version here isn't presise.  NVMe support was
	 * added to the pass(4) driver on 7/14/2017, and this particular
	 * version is from 7/22/2017, when clang 5.0 was imported.
	 */
	case PROTO_NVME: {
		struct nvme_controller_data cdata;

		bzero(&cdata, sizeof(cdata));

		bzero(&ccb, sizeof(ccb));
		cam_fill_nvmeadmin(&ccb.nvmeio, 0, NULL,
		    CAM_DIR_IN, (uint8_t *)&cdata, sizeof(cdata), 60 * 1000);
		ccb.nvmeio.cmd.opc = NVME_OPC_IDENTIFY;
		ccb.nvmeio.cmd.cdw10 = 1;

		if (cam_send_ccb(camdev, &ccb) < 0) {
			LOG(D4) << "cam_send_ccb failed";
			disk_info.devType = DEVICE_TYPE_OTHER;
			return;
		}

		if ((ccb.ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP) {
			LOG(D4) << "cam_send_ccb error" << ccb.ccb_h.status;
			IFLOG(D4)
				cam_error_print(camdev, &ccb, CAM_ESF_ALL,
				    CAM_EPF_ALL, stderr);
			disk_info.devType = DEVICE_TYPE_OTHER;
			return;
		}

		disk_info.devType = DEVICE_TYPE_NVME;
		safecopy(disk_info.serialNum, sizeof(disk_info.serialNum),
		    cdata.sn, sizeof (disk_info.serialNum));
		safecopy(disk_info.firmwareRev, sizeof(disk_info.firmwareRev),
		    cdata.fr, sizeof(cdata.fr));
		safecopy(disk_info.modelNum, sizeof(disk_info.modelNum),
		    cdata.mn, sizeof(cdata.mn));
		break;
	}
#endif
	default:
		disk_info.devType = DEVICE_TYPE_OTHER;
		break;
	}
}

/** Close the device reference so this object can be delete. */
DtaDevFreeBSDSata::~DtaDevFreeBSDSata()
{
	LOG(D1) << "Destroying DtaDevFreeBSDSata";
	cam_close_device(camdev);
}
