/*
 * Copyright (c) 1998-2002 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.2 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.  
 * Please see the License for the specific language governing rights and 
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/IOLib.h>

#include <IOKit/usb/IOUSBCommand.h>

OSDefineMetaClassAndStructors(IOUSBCommand, IOCommand)

OSDefineMetaClassAndStructors(IOUSBIsocCommand, IOCommand)

#define super	IOCommand	// same for both

IOUSBCommand*
IOUSBCommand::NewCommand()
{
    IOUSBCommand *me = new IOUSBCommand;
    
    if (me && !me->init())
    {
	me->free();
	me = NULL;
    }
    return me;
}



bool 
IOUSBCommand::init()
{
    if (!super::init())
        return false;
    // allocate our expansion data
    if (!_expansionData)
    {
	_expansionData = (ExpansionData *)IOMalloc(sizeof(ExpansionData));
	if (!_expansionData)
	    return false;
	bzero(_expansionData, sizeof(ExpansionData));
    }
    return true;
 }


void 
IOUSBCommand::free()
{
    //  This needs to be the LAST thing we do, as it disposes of our "fake" member
    //  variables.
    //
    if (_expansionData)
	IOFree(_expansionData, sizeof(ExpansionData));

    super::free();
}

// accessor methods
void 
IOUSBCommand::SetSelector(usbCommand sel) 
{
    _selector = sel;
}

void 
IOUSBCommand::SetRequest(IOUSBDeviceRequestPtr req) 
{
    _request = req;
}

void 
IOUSBCommand::SetAddress(USBDeviceAddress addr) 
{
    _address = addr;
}

void 
IOUSBCommand::SetEndpoint(UInt8 ep) 
{
    _endpoint = ep;
}

void 
IOUSBCommand::SetDirection(UInt8 dir) 
{
    _direction = dir;
}

void 
IOUSBCommand::SetType(UInt8 type) 
{
    _type = type;
}

void 
IOUSBCommand::SetBufferRounding(bool br) 
{ 
    _bufferRounding = br;
}

void 
IOUSBCommand::SetBuffer(IOMemoryDescriptor *buf) 
{	
    _buffer = buf;
}

void 
IOUSBCommand::SetUSLCompletion(IOUSBCompletion completion) 
{
    _uslCompletion = completion;
}

void 
IOUSBCommand::SetClientCompletion(IOUSBCompletion completion) 
{
    _clientCompletion = completion;
}

void 
IOUSBCommand::SetDataRemaining(UInt32 dr) 
{
    _dataRemaining = dr;
}

void 
IOUSBCommand::SetStage(UInt8 stage) 
{
    _stage = stage;
}

void 
IOUSBCommand::SetStatus(IOReturn stat) 
{
    _status = stat;
}

void 
IOUSBCommand::SetOrigBuffer(IOMemoryDescriptor *buf) 
{
    _origBuffer = buf;
}

void 
IOUSBCommand::SetDisjointCompletion(IOUSBCompletion completion) 
{
    _disjointCompletion = completion;
}

void 
IOUSBCommand::SetDblBufLength(IOByteCount len) 
{
    _dblBufLength = len;
}

void 
IOUSBCommand::SetNoDataTimeout(UInt32 to) 
{
    _noDataTimeout = to;
}

void 
IOUSBCommand::SetCompletionTimeout(UInt32 to) 
{
    _completionTimeout = to;
}

void 
IOUSBCommand::SetUIMScratch(UInt32 index, UInt32 value) 
{ 
    if (index < 10) 
        _UIMScratch[index] = value;
}

void 
IOUSBCommand::SetReqCount(IOByteCount reqCount) 
{
    _expansionData->_reqCount = reqCount;
}

usbCommand 
IOUSBCommand::GetSelector(void) 
{
    return _selector;
}

IOUSBDeviceRequestPtr 
IOUSBCommand::GetRequest(void) 
{
    return _request;
}

USBDeviceAddress IOUSBCommand::GetAddress(void) 
{
    return _address;
}

UInt8 IOUSBCommand::GetEndpoint(void) 
{
    return _endpoint;
}

UInt8 IOUSBCommand::GetDirection(void) 
{
    return _direction;
}

UInt8 IOUSBCommand::GetType(void) 
{
    return _type;
}

bool IOUSBCommand::GetBufferRounding(void) 
{
    return _bufferRounding;
}

IOMemoryDescriptor* IOUSBCommand::GetBuffer(void) 
{ 
    return _buffer;
}

IOUSBCompletion IOUSBCommand::GetUSLCompletion(void) 
{ 
    return _uslCompletion;
}

IOUSBCompletion IOUSBCommand::GetClientCompletion(void) 
{ 
    return _clientCompletion;
}

UInt32 IOUSBCommand::GetDataRemaining(void) 
{ 
    return _dataRemaining;
}

UInt8 IOUSBCommand::GetStage(void) 
{ 
    return _stage;
}

IOReturn IOUSBCommand::GetStatus(void) 
{ 
    return _status;
}

IOMemoryDescriptor * IOUSBCommand::GetOrigBuffer(void) 
{ 
    return _origBuffer;
}

IOUSBCompletion IOUSBCommand::GetDisjointCompletion(void) 
{ 
    return _disjointCompletion;
}

IOByteCount IOUSBCommand::GetDblBufLength(void) 
{ 
    return _dblBufLength;
}

UInt32 IOUSBCommand::GetNoDataTimeout(void) 
{ 
    return _noDataTimeout;
}

UInt32 IOUSBCommand::GetCompletionTimeout(void) 
{
    return _completionTimeout;
}

UInt32 IOUSBCommand::GetUIMScratch(UInt32 index) 
{ 
    return (index < 10) ? _UIMScratch[index] : 0;
}
 
IOByteCount IOUSBCommand::GetReqCount(void) 
{ 
    return _expansionData->_reqCount;
}

IOUSBIsocCommand*
IOUSBIsocCommand::NewCommand()
{
    IOUSBIsocCommand *me = new IOUSBIsocCommand;
    
    if (me && !me->init())
    {
	me->free();
	me = NULL;
    }
    return me;
}

bool 
IOUSBIsocCommand::init()
{
    if (!super::init())
        return false;
    // allocate our expansion data
    if (!_expansionData)
    {
	_expansionData = (ExpansionData *)IOMalloc(sizeof(ExpansionData));
	if (!_expansionData)
	    return false;
	bzero(_expansionData, sizeof(ExpansionData));
    }
    return true;
 }


void 
IOUSBIsocCommand::free()
{
    //  This needs to be the LAST thing we do, as it disposes of our "fake" member
    //  variables.
    //
    if (_expansionData)
	IOFree(_expansionData, sizeof(ExpansionData));

    super::free();
}


