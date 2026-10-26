/*
 * Native eMMC block driver for the Xiaomi sirius (SDM710 / sdm670 SDCC1).
 *
 * The transport itself lives in sirius_emmc.c, which the boot probe also uses: that file owns the
 * controller bring-up, the 400 kHz card clock, the ADMA2 transfer and the cache maintenance the
 * data engine needs. This file only turns it into an IOService plus an IOBlockStorageDevice so
 * IOStorageFamily publishes IOMedia and the kernel can find the root window.
 *
 * Read-only by construction: the transfer path has no write command and reportWriteProtection()
 * says so.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>

extern "C" {
/* From sirius_emmc.c. prepare() is idempotent and also fills total_blocks from the GPT. */
int sirius_emmc_prepare(void);
int sirius_emmc_read_block(unsigned long long lba, void *buffer);
extern unsigned long long sirius_emmc_total_blocks;
}

#define EMMC_BLOCK_SIZE 512

class NativeEmmcBlock : public IOBlockStorageDevice
{
	OSDeclareDefaultStructors(NativeEmmcBlock);

	UInt64 fBlocks;
	IOLock *fLock;
	uint8_t *fBounce;

public:
	bool init(UInt64 blocks);
	bool start(IOService *provider) override;
	void free() override;

	IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
	IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
	UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
	char *getVendorString() override { return const_cast<char *>("Xiaomi"); }
	char *getProductString() override { return const_cast<char *>("sirius eMMC"); }
	char *getRevisionString() override { return const_cast<char *>("1.0"); }
	char *getAdditionalDeviceInfoString() override
	{
		return const_cast<char *>("SDCC1 eMMC, read-only, ADMA2");
	}
	IOReturn reportBlockSize(UInt64 *value) override
	{
		*value = EMMC_BLOCK_SIZE;
		return kIOReturnSuccess;
	}
	IOReturn reportEjectability(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportMaxValidBlock(UInt64 *value) override
	{
		*value = fBlocks ? fBlocks - 1 : 0;
		return kIOReturnSuccess;
	}
	IOReturn reportMediaState(bool *present, bool *changed) override
	{
		*present = fBlocks != 0;
		if (changed) {
			*changed = false;
		}
		return kIOReturnSuccess;
	}
	IOReturn reportRemovability(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn reportWriteProtection(bool *value) override
	{
		*value = true;
		return kIOReturnSuccess;
	}
	IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
	IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
	IOReturn getWriteCacheState(bool *value) override
	{
		*value = false;
		return kIOReturnSuccess;
	}
	IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }

	IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
	    IOStorageAttributes *attributes, IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(NativeEmmcBlock, IOBlockStorageDevice);

bool
NativeEmmcBlock::init(UInt64 blocks)
{
	if (!IOBlockStorageDevice::init(nullptr)) {
		return false;
	}
	fBlocks = blocks;
	fLock = IOLockAlloc();
	fBounce = (uint8_t *)IOMalloc(EMMC_BLOCK_SIZE);
	if (!fLock || !fBounce) {
		return false;
	}
	return true;
}

bool
NativeEmmcBlock::start(IOService *provider)
{
	if (!fLock || !fBounce || !IOBlockStorageDevice::start(provider)) {
		return false;
	}
	registerService();
	return true;
}

void
NativeEmmcBlock::free()
{
	if (fBounce) {
		IOFree(fBounce, EMMC_BLOCK_SIZE);
		fBounce = nullptr;
	}
	if (fLock) {
		IOLockFree(fLock);
		fLock = nullptr;
	}
	IOBlockStorageDevice::free();
}

IOReturn
NativeEmmcBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
	(void)attributes;
	if (!buffer || !completion || !completion->action || count == 0) {
		return kIOReturnBadArgument;
	}
	if (block >= fBlocks || count > fBlocks - block) {
		return kIOReturnBadArgument;
	}
	if (buffer->getDirection() != kIODirectionIn) {
		return kIOReturnNotWritable;
	}
	if (buffer->getLength() < count * EMMC_BLOCK_SIZE) {
		return kIOReturnBadArgument;
	}

	IOReturn result = kIOReturnSuccess;
	IOLockLock(fLock);
	for (UInt64 i = 0; i < count; i++) {
		if (sirius_emmc_read_block(block + i, fBounce) != 0) {
			result = kIOReturnIOError;
			break;
		}
		if (buffer->writeBytes(i * EMMC_BLOCK_SIZE, fBounce, EMMC_BLOCK_SIZE) !=
		    EMMC_BLOCK_SIZE) {
			result = kIOReturnIOError;
			break;
		}
	}
	IOLockUnlock(fLock);

	/* The transfer is synchronous, so the completion fires before returning. */
	completion->action(completion->target, completion->parameter, result,
	    result == kIOReturnSuccess ? count * EMMC_BLOCK_SIZE : 0);
	return result;
}

class NativeEmmc : public IOService
{
	OSDeclareDefaultStructors(NativeEmmc);

	NativeEmmcBlock *fBlock;

public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
};

OSDefineMetaClassAndStructors(NativeEmmc, IOService);

bool
NativeEmmc::start(IOService *provider)
{
	fBlock = nullptr;
	if (!IOService::start(provider)) {
		return false;
	}
	if (sirius_emmc_prepare() != 0) {
		IOLog("NativeEmmc: the eMMC did not come up\n");
		return false;
	}
	UInt64 blocks = sirius_emmc_total_blocks;
	if (blocks == 0) {
		IOLog("NativeEmmc: no usable GPT on the device\n");
		return false;
	}

	fBlock = new NativeEmmcBlock;
	if (!fBlock || !fBlock->init(blocks)) {
		IOLog("NativeEmmc: cannot allocate the block device\n");
		return false;
	}
	fBlock->attach(this);
	if (!fBlock->start(this)) {
		IOLog("NativeEmmc: the block device did not start\n");
		return false;
	}
	IOLog("NativeEmmc: published %llu blocks of %u bytes\n", blocks, EMMC_BLOCK_SIZE);
	registerService();
	return true;
}

void
NativeEmmc::stop(IOService *provider)
{
	if (fBlock) {
		fBlock->terminate(kIOServiceSynchronous);
		fBlock->release();
		fBlock = nullptr;
	}
	IOService::stop(provider);
}
