/*
 * Intel ACPI Component Architecture
 * AML/ASL+ Disassembler version 20260408 (64-bit version)
 * Copyright (c) 2000 - 2026 Intel Corporation
 * 
 * Disassembling to symbolic ASL+ operators
 *
 * Disassembly of SSDT9.aml
 *
 * Original Table Header:
 *     Signature        "SSDT"
 *     Length           0x00000295 (661)
 *     Revision         0x02
 *     Checksum         0xB5
 *     OEM ID           "PmRef"
 *     OEM Table ID     "Cpu0Cst"
 *     OEM Revision     0x00003001 (12289)
 *     Compiler ID      "INTL"
 *     Compiler Version 0x20140424 (538182692)
 */
DefinitionBlock ("", "SSDT", 2, "PmRef", "Cpu0Cst", 0x00003001)
{
    External (_PR_.CPU0, ProcessorObj)
    External (C3LT, IntObj)
    External (C3MW, IntObj)
    External (C6LT, IntObj)
    External (C6MW, IntObj)
    External (C7LT, IntObj)
    External (C7MW, IntObj)
    External (CDLT, IntObj)
    External (CDLV, IntObj)
    External (CDMW, IntObj)
    External (CDPW, IntObj)
    External (CFGD, UnknownObj)
    External (PDC0, UnknownObj)

    Scope (\_PR.CPU0)
    {
        Name (C1TM, Package (0x04)
        {
            Buffer (0x11)
            {
                /* 0000 */  0x82, 0x0C, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00,  // ........
                /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,  // .......y
                /* 0010 */  0x00                                             // .
            }, 

            One, 
            One, 
            0x03E8
        })
        Name (C3TM, Package (0x04)
        {
            Buffer (0x11)
            {
                /* 0000 */  0x82, 0x0C, 0x00, 0x01, 0x08, 0x00, 0x00, 0x14,  // ........
                /* 0008 */  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,  // .......y
                /* 0010 */  0x00                                             // .
            }, 

            0x02, 
            Zero, 
            0x01F4
        })
        Name (C6TM, Package (0x04)
        {
            Buffer (0x11)
            {
                /* 0000 */  0x82, 0x0C, 0x00, 0x01, 0x08, 0x00, 0x00, 0x15,  // ........
                /* 0008 */  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,  // .......y
                /* 0010 */  0x00                                             // .
            }, 

            0x02, 
            Zero, 
            0x015E
        })
        Name (C7TM, Package (0x04)
        {
            Buffer (0x11)
            {
                /* 0000 */  0x82, 0x0C, 0x00, 0x01, 0x08, 0x00, 0x00, 0x16,  // ........
                /* 0008 */  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,  // .......y
                /* 0010 */  0x00                                             // .
            }, 

            0x02, 
            Zero, 
            0xC8
        })
        Name (CDTM, Package (0x04)
        {
            Buffer (0x11)
            {
                /* 0000 */  0x82, 0x0C, 0x00, 0x01, 0x08, 0x00, 0x00, 0x16,  // ........
                /* 0008 */  0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,  // .......y
                /* 0010 */  0x00                                             // .
            }, 

            0x03, 
            Zero, 
            Zero
        })
        Name (MWES, Buffer (0x11)
        {
            /* 0000 */  0x82, 0x0C, 0x00, 0x7F, 0x01, 0x02, 0x01, 0x00,  // ........
            /* 0008 */  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x79,  // .......y
            /* 0010 */  0x00                                             // .
        })
        Name (AC2V, Zero)
        Name (AC3V, Zero)
        Name (C3ST, Package (0x04)
        {
            0x03, 
            Package (0x01)
            {
                Zero
            }, 

            Package (0x01)
            {
                Zero
            }, 

            Package (0x01)
            {
                Zero
            }
        })
        Name (C2ST, Package (0x03)
        {
            0x02, 
            Package (0x01)
            {
                Zero
            }, 

            Package (0x01)
            {
                Zero
            }
        })
        Name (C1ST, Package (0x02)
        {
            One, 
            Package (0x01)
            {
                Zero
            }
        })
        Name (CSTF, Zero)
        Method (_CST, 0, Serialized)  // _CST: C-States
        {
            If (!CSTF)
            {
                C3TM [0x02] = C3LT /* External reference */
                C6TM [0x02] = C6LT /* External reference */
                C7TM [0x02] = C7LT /* External reference */
                CDTM [0x02] = CDLT /* External reference */
                CDTM [0x03] = CDPW /* External reference */
                DerefOf (CDTM [Zero]) [0x07] = CDLV /* External reference */
                If (((CFGD & 0x0800) && (PDC0 & 0x0200)))
                {
                    C1TM [Zero] = MWES /* \_PR_.CPU0.MWES */
                    C3TM [Zero] = MWES /* \_PR_.CPU0.MWES */
                    C6TM [Zero] = MWES /* \_PR_.CPU0.MWES */
                    C7TM [Zero] = MWES /* \_PR_.CPU0.MWES */
                    CDTM [Zero] = MWES /* \_PR_.CPU0.MWES */
                    DerefOf (C3TM [Zero]) [0x07] = C3MW /* External reference */
                    DerefOf (C6TM [Zero]) [0x07] = C6MW /* External reference */
                    DerefOf (C7TM [Zero]) [0x07] = C7MW /* External reference */
                    DerefOf (CDTM [Zero]) [0x07] = CDMW /* External reference */
                }
                ElseIf (((CFGD & 0x0800) && (PDC0 & 0x0100)))
                {
                    C1TM [Zero] = MWES /* \_PR_.CPU0.MWES */
                }

                CSTF = Ones
            }

            AC2V = Zero
            AC3V = Zero
            C3ST [One] = C1TM /* \_PR_.CPU0.C1TM */
            C3ST [0x02] = C3TM /* \_PR_.CPU0.C3TM */
            C3ST [0x03] = C6TM /* \_PR_.CPU0.C6TM */
            Return (C3ST) /* \_PR_.CPU0.C3ST */
        }
    }
}

