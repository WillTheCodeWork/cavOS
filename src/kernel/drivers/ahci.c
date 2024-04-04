#include <ahci.h>
#include <bootloader.h>
#include <isr.h>
#include <linked_list.h>
#include <malloc.h>
#include <paging.h>
#include <pmm.h>
#include <string.h>
#include <timer.h>
#include <util.h>
#include <vmm.h>

// Generic AHCI driver (tested on real hardware)
// Tried to keep the code as simple as I could, good for educational purposes
// Copyright (C) 2024 Panagiotis

AHCI_DEVICE *isAHCIcontroller(PCIdevice *device) {
  for (int i = 0; i < (sizeof(ahci_ids) / sizeof(ahci_ids[0])); i++) {
    if (COMBINE_WORD(device->device_id, device->vendor_id) == ahci_ids[i].id)
      return &ahci_ids[i];
  }

  return 0;
}

// Start command engine
void start_cmd(HBA_PORT *port) {
  // Wait until CR (bit15) is cleared
  while (port->cmd & HBA_PxCMD_CR)
    ;

  // Set FRE (bit4) and ST (bit0)
  port->cmd |= HBA_PxCMD_FRE;
  port->cmd |= HBA_PxCMD_ST;
}

// Stop command engine
void stop_cmd(HBA_PORT *port) {
  // Clear ST (bit0)
  port->cmd &= ~HBA_PxCMD_ST;

  // Clear FRE (bit4)
  port->cmd &= ~HBA_PxCMD_FRE;

  // Wait until FR (bit14), CR (bit15) are cleared
  while (1) {
    if (port->cmd & HBA_PxCMD_FR)
      continue;
    if (port->cmd & HBA_PxCMD_CR)
      continue;
    break;
  }
}

int find_cmdslot(HBA_PORT *port) {
  // If not set in SACT and CI, the slot is free
  uint32_t slots = (port->sact | port->ci);
  for (int i = 0; i < 1; i++) { // todo: we got more than one lol
    if ((slots & 1) == 0)
      return i;
    slots >>= 1;
  }
  printf("Cannot find free command list entry\n");
  return -1;
}

bool ahciRead(ahci *ahciPtr, uint32_t portId, HBA_PORT *port, uint32_t startl,
              uint32_t starth, uint32_t count, uint16_t *buf) {
  port->is = (uint32_t)-1; // Clear pending interrupt bits
  int spin = 0;            // Spin lock timeout counter
  int slot = find_cmdslot(port);
  // printf("slot: %d\n", slot);
  if (slot == -1)
    return false;

  HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)ahciPtr->clbVirt[portId];
  cmdheader = (size_t)cmdheader + slot * sizeof(HBA_CMD_HEADER);
  cmdheader->cfl = sizeof(FIS_REG_H2D) / sizeof(uint32_t); // Command FIS size
  cmdheader->w = 0;                                        // Read from device
  cmdheader->prdtl = (uint16_t)((count - 1) >> 4) + 1;     // PRDT entries count
  // printf("prdt:%d\n", (uint16_t)((count - 1) >> 4) + 1);

  HBA_CMD_TBL *cmdtbl = (HBA_CMD_TBL *)ahciPtr->ctbaVirt[portId];
  memset(cmdtbl, 0,
         sizeof(HBA_CMD_TBL) + (cmdheader->prdtl - 1) * sizeof(HBA_PRDT_ENTRY));

  // 8K bytes (16 sectors) per PRDT
  int    i = 0;
  size_t targPhys = VirtualToPhysical(buf);
  for (i = 0; i < cmdheader->prdtl - 1; i++) {
    cmdtbl->prdt_entry[i].dba = (uint32_t)(targPhys & 0xFFFFFFFF);
    cmdtbl->prdt_entry[i].dbau = (uint32_t)(targPhys >> 32);
    cmdtbl->prdt_entry[i].dbc =
        8 * 1024 - 1; // 8K bytes (this value should always be set to 1 less
                      // than the actual value)
    cmdtbl->prdt_entry[i].i = 1;
    buf += 4 * 1024; // 4K words
    count -= 16;     // 16 sectors
  }
  // Last entry
  cmdtbl->prdt_entry[i].dba = (uint32_t)(targPhys & 0xFFFFFFFF);
  cmdtbl->prdt_entry[i].dbau = (uint32_t)(targPhys >> 32);
  cmdtbl->prdt_entry[i].dbc = (count << 9) - 1; // 512 bytes per sector
  cmdtbl->prdt_entry[i].i = 1;

  // Setup command
  // printf("cfis: %x\n", cmdtbl->cfis);
  FIS_REG_H2D *cmdfis = (FIS_REG_H2D *)(&cmdtbl->cfis);

  cmdfis->fis_type = FIS_TYPE_REG_H2D;
  cmdfis->c = 1; // Command
  cmdfis->command = ATA_CMD_READ_DMA_EX;

  cmdfis->lba0 = (uint8_t)startl;
  cmdfis->lba1 = (uint8_t)(startl >> 8);
  cmdfis->lba2 = (uint8_t)(startl >> 16);
  cmdfis->device = 1 << 6; // LBA mode

  cmdfis->lba3 = (uint8_t)(startl >> 24);
  cmdfis->lba4 = (uint8_t)starth;
  cmdfis->lba5 = (uint8_t)(starth >> 8);

  cmdfis->countl = count & 0xFF;
  cmdfis->counth = (count >> 8) & 0xFF;

  // The below loop waits until the port is no longer busy before issuing a new
  // command
  while ((port->tfd & (ATA_DEV_BUSY | ATA_DEV_DRQ)) && spin < 1000000) {
    spin++;
  }
  if (spin == 1000000) {
    printf("[pci::ahci] Port is hung ATA_DEV_BUSY{%d} ATA_DEV_DRQ{%d}\n",
           port->tfd & ATA_DEV_BUSY, port->tfd & ATA_DEV_DRQ);
    return false;
  }

  port->ci = 1 << slot; // Issue command

  // Wait for completion
  while (1) {
    // In some longer duration reads, it may be helpful to spin on the DPS bit
    // in the PxIS port field as well (1 << 5)
    if ((port->ci & (1 << slot)) == 0)
      break;
    if (port->is & HBA_PxIS_TFES) // Task file error
    {
      printf("[pci::ahci] Read disk error\n");
      return false;
    }
  }

  // Check again
  if (port->is & HBA_PxIS_TFES) {
    printf("[pci::ahci] Read disk error\n");
    return false;
  }

  return true;
}

void port_rebase(ahci *ahciPtr, HBA_PORT *port, int portno) {
  stop_cmd(port); // Stop command engine

  // enable (all) interrupts
  port->ie = 1;

  // Command list offset: 1K*portno
  // Command list entry size = 32
  // Command list entry maxim count = 32
  // Command list maxim size = 32*32 = 1K per port
  uint32_t clbPages = DivRoundUp(sizeof(HBA_CMD_HEADER) * 32, BLOCK_SIZE);
  void    *clbVirt = VirtualAllocate(clbPages); //!
  ahciPtr->clbVirt[portno] = clbVirt;
  size_t clbPhys = VirtualToPhysical(clbVirt);
  port->clb = (uint32_t)(clbPhys & 0xFFFFFFFF);
  port->clbu = (uint32_t)(clbPhys >> 32);
  memset(clbVirt, 0, clbPages * BLOCK_SIZE); // could've just done 1024

  // FIS offset: 32K+256*portno
  // FIS entry size = 256 bytes per port
  // use a bit of the wasted (256-aligned) space for this
  size_t fbPhys = clbPhys + 2048;
  port->fb = (uint32_t)(fbPhys & 0xFFFFFFFF);
  port->fbu = (uint32_t)(fbPhys >> 32);
  // memset((void *)(port->fb), 0, 256); already 0'd

  // Command table offset: 40K + 8K*portno
  // Command table size = 256*32 = 8K per port
  HBA_CMD_HEADER *cmdheader = (HBA_CMD_HEADER *)clbVirt;
  void           *ctbaVirt = VirtualAllocate(2); // 2 pages = 8192 bytes //!
  ahciPtr->ctbaVirt[portno] = ctbaVirt;
  size_t ctbaPhys = (size_t)VirtualToPhysical(ctbaVirt);
  memset(ctbaVirt, 0, 8192);
  for (int i = 0; i < 32; i++) {
    cmdheader[i].prdtl = 8; // 8 prdt entries per command table
                            // 256 bytes per command table, 64+16+48+16*8
    // Command table offset: 40K + 8K*portno + cmdheader_index*256
    size_t ctbaPhysCurr = ctbaPhys + i * 256;
    cmdheader[i].ctba = (uint32_t)(ctbaPhysCurr & 0xFFFFFFFF);
    cmdheader[i].ctbau = (uint32_t)(ctbaPhysCurr >> 32);
    // memset((void *)cmdheader[i].ctba, 0, 256); already 0'd
  }

  if (port->serr & (1 << 10))
    port->serr |= (1 << 10);

  // COMRESET / CLO*
  port->cmd |= (1 << 3);
  uint64_t targ = timerTicks + 150;
  while (port->cmd & (1 << 3) && timerTicks < targ)
    ;

  // OpenBSD hack (made pavilion work)
  port->serr = port->serr;
  start_cmd(port); // Start command engine

  ahciPtr->sata |= (1 << portno);
}

// Check device type
int check_type(HBA_PORT *port) {
  uint32_t ssts = port->ssts;

  uint8_t ipm = (ssts >> 8) & 0x0F;
  uint8_t det = ssts & 0x0F;

  if (det != HBA_PORT_DET_PRESENT) // Check drive status
    return AHCI_DEV_NULL;
  if (ipm != HBA_PORT_IPM_ACTIVE)
    return AHCI_DEV_NULL;

  switch (port->sig) {
  case SATA_SIG_ATAPI:
    return AHCI_DEV_SATAPI;
  case SATA_SIG_SEMB:
    return AHCI_DEV_SEMB;
  case SATA_SIG_PM:
    return AHCI_DEV_PM;
  default:
    return AHCI_DEV_SATA;
  }
}

void probe_port(ahci *ahciPtr, HBA_MEM *abar) {
  uint32_t pi = abar->pi;
  int      i = 0;
  while (i < 32) {
    if (pi & 1) {
      int dt = check_type(&abar->ports[i]);
      if (dt == AHCI_DEV_SATA) {
        debugf("[pci::ahci] SATA drive found at port %d\n", i);
        port_rebase(ahciPtr, &abar->ports[i], i);
      } else if (dt == AHCI_DEV_SATAPI) {
        debugf("[pci::ahci] (unsupported) SATAPI drive found at port %d\n", i);
      } else if (dt == AHCI_DEV_SEMB) {
        debugf("[pci::ahci] (unsupported) SEMB drive found at port %d\n", i);
      } else if (dt == AHCI_DEV_PM) {
        debugf("[pci::ahci] (unsupported) PM drive found at port %d\n", i);
      }
      // otherwise, no drive is in this port
    }

    pi >>= 1;
    i++;
  }
}

void ahciInterruptHandler(AsmPassedInterrupt *regs) {
  PCI *browse = firstPCI;
  while (browse) {
    if (browse->driver == PCI_DRIVER_AHCI) {
      ahci *ahciPtr = browse->extra;
      // printf("[pci::ahci] Interrupt hit!\n");
      ahciPtr->mem->is = ahciPtr->mem->is;
      ahciPtr->mem->ports[0].is = ahciPtr->mem->ports[0].is;
    }

    browse = browse->next;
  }
}

bool initiateAHCI(PCIdevice *device) {
  AHCI_DEVICE *ahciDevice = isAHCIcontroller(device);
  if (!ahciDevice)
    return false;

  debugf("[pci::ahci] Detected controller! name{%s} quirks{%x}\n",
         ahciDevice->name, ahciDevice->quirks);

  PCIgeneralDevice *details =
      (PCIgeneralDevice *)malloc(sizeof(PCIgeneralDevice));
  GetGeneralDevice(device, details);
  uint32_t base = details->bar[5] & 0xFFFFFFF0;

  // Enable PCI Bus Mastering, memory access and interrupts (if not already)
  uint32_t command_status = COMBINE_WORD(device->status, device->command);
  if (!(command_status & (1 << 2)))
    command_status |= (1 << 2);
  if (!(command_status & (1 << 1)))
    command_status |= (1 << 1);
  if (command_status & (1 << 10))
    command_status |= (1 << 10);
  ConfigWriteDword(device->bus, device->slot, device->function, PCI_COMMAND,
                   command_status);

  PCI *pci = lookupPCIdevice(device);
  setupPCIdeviceDriver(pci, PCI_DRIVER_AHCI, PCI_DRIVER_CATEGORY_STORAGE);

  ahci *ahciPtr = (ahci *)malloc(sizeof(ahci));
  pci->extra = ahciPtr;

  HBA_MEM *mem = bootloader.hhdmOffset + base; //!

  ahciPtr->bsdInfo = ahciDevice;
  ahciPtr->mem = mem;
  uint32_t size = strlength(ahciDevice->name) + 1; // null terminated
  pci->name = (char *)malloc(size);
  memcpy(pci->name, ahciDevice->name, size);

  // do a full HBA reset (as per 10.4.3)
  mem->ghc |= (1 << 0);
  while (mem->ghc & (1 << 0))
    ;
  // printf("[pci::ahci] Reset successfully!\n");

  if (!(mem->bohc & 2) || mem->cap2 & AHCI_BIOS_OWNED) {
    debugf("[pci::ahci] Performing BIOS->OS handoff required!\n");
    // mem->bohc |= AHCI_OS_OWNED;
    // while (mem->bohc & AHCI_OS_OWNED)
    //   ;
    // sleep(50);
    // if (mem->bohc & AHCI_BIOS_BUSY)
    //   sleep(2000);
    mem->bohc = (mem->bohc & ~8) | 2;
    while ((mem->bohc & 1))
      ;
    if ((mem->bohc & 1)) {
      // forcibly take it
      mem->bohc = 2;
      mem->bohc |= 8;
    }
  }

  if (!(mem->ghc & (1 << 31)))
    mem->ghc |= (1 << 31);

  probe_port(ahciPtr, mem);

  // enable interrupts
  pci->irqHandler =
      registerIRQhandler(details->interruptLine, &ahciInterruptHandler);
  if (!(mem->ghc & (1 << 1)))
    mem->ghc |= 1 << 1;

  // printf("[pci::ahci] somehow... is{%d} version{%d} sss{%d}\n", mem->is,
  //        mem->vs, mem->cap & (1 << 27));

  // uint8_t *buff = (uint8_t *)malloc(512);
  // ahciRead(&mem->ports[0], 0, 0, 1, buff);
  // for (int i = 0; i < 512; i++) {
  //   printf("%02X ", buff[i]);
  // }
  // printf("\n");

  return true;
}
