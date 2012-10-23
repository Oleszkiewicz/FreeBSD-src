/******************************************************************************
 *
 * Module Name: exconfig - Namespace reconfiguration (Load/Unload opcodes)
 *
 *****************************************************************************/

/*
 * Copyright (C) 2000 - 2012, Intel Corp.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer,
 *    without modification.
 * 2. Redistributions in binary form must reproduce at minimum a disclaimer
 *    substantially similar to the "NO WARRANTY" disclaimer below
 *    ("Disclaimer") and any redistribution must be conditioned upon
 *    including a substantially similar Disclaimer requirement for further
 *    binary redistribution.
 * 3. Neither the names of the above-listed copyright holders nor the names
 *    of any contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * Alternatively, this software may be distributed under the terms of the
 * GNU General Public License ("GPL") version 2 as published by the Free
 * Software Foundation.
 *
 * NO WARRANTY
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGES.
 */

#define __EXCONFIG_C__

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>
#include <contrib/dev/acpica/include/acinterp.h>
#include <contrib/dev/acpica/include/acnamesp.h>
#include <contrib/dev/acpica/include/actables.h>
#include <contrib/dev/acpica/include/acdispat.h>
#include <contrib/dev/acpica/include/acevents.h>


#define _COMPONENT          ACPI_EXECUTER
        ACPI_MODULE_NAME    ("exconfig")

/* Local prototypes */

static ACPI_STATUS
AcpiExAddTable (
    UINT32                  TableIndex,
    ACPI_NAMESPACE_NODE     *ParentNode,
    ACPI_OPERAND_OBJECT     **DdbHandle);

static ACPI_STATUS
AcpiExRegionRead (
    ACPI_OPERAND_OBJECT     *ObjDesc,
    UINT32                  Length,
    UINT8                   *Buffer);


/*******************************************************************************
 *
 * FUNCTION:    AcpiExAddTable
 *
 * PARAMETERS:  Table               - Pointer to raw table
 *              ParentNode          - Where to load the table (scope)
 *              DdbHandle           - Where to return the table handle.
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Common function to Install and Load an ACPI table with a
 *              returned table handle.
 *
 ******************************************************************************/

static ACPI_STATUS
AcpiExAddTable (
    UINT32                  TableIndex,
    ACPI_NAMESPACE_NODE     *ParentNode,
    ACPI_OPERAND_OBJECT     **DdbHandle)
{
    ACPI_OPERAND_OBJECT     *ObjDesc;
    ACPI_STATUS             Status;
    ACPI_OWNER_ID           OwnerId;


    ACPI_FUNCTION_TRACE (ExAddTable);


    /* Create an object to be the table handle */

    ObjDesc = AcpiUtCreateInternalObject (ACPI_TYPE_LOCAL_REFERENCE);
    if (!ObjDesc)
    {
        return_ACPI_STATUS (AE_NO_MEMORY);
    }

    /* Init the table handle */

    ObjDesc->Common.Flags |= AOPOBJ_DATA_VALID;
    ObjDesc->Reference.Class = ACPI_REFCLASS_TABLE;
    *DdbHandle = ObjDesc;

    /* Install the new table into the local data structures */

    ObjDesc->Reference.Value = TableIndex;

    /* Add the table to the namespace */

    Status = AcpiNsLoadTable (TableIndex, ParentNode);
    if (ACPI_FAILURE (Status))
    {
        AcpiUtRemoveReference (ObjDesc);
        *DdbHandle = NULL;
        return_ACPI_STATUS (Status);
    }

    /* Execute any module-level code that was found in the table */

    AcpiExExitInterpreter ();
    AcpiNsExecModuleCodeList ();
    AcpiExEnterInterpreter ();

    /*
     * Update GPEs for any new _Lxx/_Exx methods. Ignore errors. The host is
     * responsible for discovering any new wake GPEs by running _PRW methods
     * that may have been loaded by this table.
     */
    Status = AcpiTbGetOwnerId (TableIndex, &OwnerId);
    if (ACPI_SUCCESS (Status))
    {
        AcpiEvUpdateGpes (OwnerId);
    }

    return_ACPI_STATUS (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiExLoadTableOp
 *
 * PARAMETERS:  WalkState           - Current state with operands
 *              ReturnDesc          - Where to store the return object
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load an ACPI table from the RSDT/XSDT
 *
 ******************************************************************************/

ACPI_STATUS
AcpiExLoadTableOp (
    ACPI_WALK_STATE         *WalkState,
    ACPI_OPERAND_OBJECT     **ReturnDesc)
{
    ACPI_STATUS             Status;
    ACPI_OPERAND_OBJECT     **Operand = &WalkState->Operands[0];
    ACPI_NAMESPACE_NODE     *ParentNode;
    ACPI_NAMESPACE_NODE     *StartNode;
    ACPI_NAMESPACE_NODE     *ParameterNode = NULL;
    ACPI_OPERAND_OBJECT     *DdbHandle;
    ACPI_TABLE_HEADER       *Table;
    UINT32                  TableIndex;


    ACPI_FUNCTION_TRACE (ExLoadTableOp);


    /* Validate lengths for the Signature, OemId, and OemTableId strings */

    if ((Operand[0]->String.Length > ACPI_NAME_SIZE) ||
        (Operand[1]->String.Length > ACPI_OEM_ID_SIZE) ||
        (Operand[2]->String.Length > ACPI_OEM_TABLE_ID_SIZE))
    {
        return_ACPI_STATUS (AE_BAD_PARAMETER);
    }

    /* Find the ACPI table in the RSDT/XSDT */

    Status = AcpiTbFindTable (Operand[0]->String.Pointer,
                              Operand[1]->String.Pointer,
                              Operand[2]->String.Pointer, &TableIndex);
    if (ACPI_FAILURE (Status))
    {
        if (Status != AE_NOT_FOUND)
        {
            return_ACPI_STATUS (Status);
        }

        /* Table not found, return an Integer=0 and AE_OK */

        DdbHandle = AcpiUtCreateIntegerObject ((UINT64) 0);
        if (!DdbHandle)
        {
            return_ACPI_STATUS (AE_NO_MEMORY);
        }

        *ReturnDesc = DdbHandle;
        return_ACPI_STATUS (AE_OK);
    }

    /* Default nodes */

    StartNode = WalkState->ScopeInfo->Scope.Node;
    ParentNode = AcpiGbl_RootNode;

    /* RootPath (optional parameter) */

    if (Operand[3]->String.Length > 0)
    {
        /*
         * Find the node referenced by the RootPathString. This is the
         * location within the namespace where the table will be loaded.
         */
        Status = AcpiNsGetNode (StartNode, Operand[3]->String.Pointer,
                    ACPI_NS_SEARCH_PARENT, &ParentNode);
        if (ACPI_FAILURE (Status))
        {
            return_ACPI_STATUS (Status);
        }
    }

    /* ParameterPath (optional parameter) */

    if (Operand[4]->String.Length > 0)
    {
        if ((Operand[4]->String.Pointer[0] != '\\') &&
            (Operand[4]->String.Pointer[0] != '^'))
        {
            /*
             * Path is not absolute, so it will be relative to the node
             * referenced by the RootPathString (or the NS root if omitted)
             */
            StartNode = ParentNode;
        }

        /* Find the node referenced by the ParameterPathString */

        Status = AcpiNsGetNode (StartNode, Operand[4]->String.Pointer,
                    ACPI_NS_SEARCH_PARENT, &ParameterNode);
        if (ACPI_FAILURE (Status))
        {
            return_ACPI_STATUS (Status);
        }
    }

    /* Load the table into the namespace */

    Status = AcpiExAddTable (TableIndex, ParentNode, &DdbHandle);
    if (ACPI_FAILURE (Status))
    {
        return_ACPI_STATUS (Status);
    }

    /* Parameter Data (optional) */

    if (ParameterNode)
    {
        /* Store the parameter data into the optional parameter object */

        Status = AcpiExStore (Operand[5],
                    ACPI_CAST_PTR (ACPI_OPERAND_OBJECT, ParameterNode),
                    WalkState);
        if (ACPI_FAILURE (Status))
        {
            (void) AcpiExUnloadTable (DdbHandle);

            AcpiUtRemoveReference (DdbHandle);
            return_ACPI_STATUS (Status);
        }
    }

    Status = AcpiGetTableByIndex (TableIndex, &Table);
    if (ACPI_SUCCESS (Status))
    {
        ACPI_INFO ((AE_INFO, "Dynamic OEM Table Load:"));
        AcpiTbPrintTableHeader (0, Table);
    }

    /* Invoke table handler if present */

    if (AcpiGbl_TableHandler)
    {
        (void) AcpiGbl_TableHandler (ACPI_TABLE_EVENT_LOAD, Table,
                    AcpiGbl_TableHandlerContext);
    }

    *ReturnDesc = DdbHandle;
    return_ACPI_STATUS  (Status);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiExRegionRead
 *
 * PARAMETERS:  ObjDesc         - Region descriptor
 *              Length          - Number of bytes to read
 *              Buffer          - Pointer to where to put the data
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Read data from an operation region. The read starts from the
 *              beginning of the region.
 *
 ******************************************************************************/

static ACPI_STATUS
AcpiExRegionRead (
    ACPI_OPERAND_OBJECT     *ObjDesc,
    UINT32                  Length,
    UINT8                   *Buffer)
{
    ACPI_STATUS             Status;
    UINT64                  Value;
    UINT32                  RegionOffset = 0;
    UINT32                  i;


    /* Bytewise reads */

    for (i = 0; i < Length; i++)
    {
        Status = AcpiEvAddressSpaceDispatch (ObjDesc, NULL, ACPI_READ,
                    RegionOffset, 8, &Value);
        if (ACPI_FAILURE (Status))
        {
            return (Status);
        }

        *Buffer = (UINT8) Value;
        Buffer++;
        RegionOffset++;
    }

    return (AE_OK);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiExLoadOp
 *
 * PARAMETERS:  ObjDesc         - Region or Buffer/Field where the table will be
 *                                obtained
 *              Target          - Where a handle to the table will be stored
 *              WalkState       - Current state
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load an ACPI table from a field or operation region
 *
 * NOTE: Region Fields (Field, BankField, IndexFields) are resolved to buffer
 *       objects before this code is reached.
 *
 *       If source is an operation region, it must refer to SystemMemory, as
 *       per the ACPI specification.
 *
 ******************************************************************************/

ACPI_STATUS
AcpiExLoadOp (
    ACPI_OPERAND_OBJECT     *ObjDesc,
    ACPI_OPERAND_OBJECT     *Target,
    ACPI_WALK_STATE         *WalkState)
{
    ACPI_OPERAND_OBJECT     *DdbHandle;
    ACPI_TABLE_HEADER       *Table;
    ACPI_TABLE_DESC         TableDesc;
    UINT32                  TableIndex;
    ACPI_STATUS             Status;
    UINT32                  Length;


    ACPI_FUNCTION_TRACE (ExLoadOp);


    ACPI_MEMSET (&TableDesc, 0, sizeof (ACPI_TABLE_DESC));

    /* Source Object can be either an OpRegion or a Buffer/Field */

    switch (ObjDesc->Common.Type)
    {
    case ACPI_TYPE_REGION:

        ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
            "Load table from Region %p\n", ObjDesc));

        /* Region must be SystemMemory (from ACPI spec) */

        if (ObjDesc->Region.SpaceId != ACPI_ADR_SPACE_SYSTEM_MEMORY)
        {
            return_ACPI_STATUS (AE_AML_OPERAND_TYPE);
        }

        /*
         * If the Region Address and Length have not been previously evaluated,
         * evaluate them now and save the results.
         */
        if (!(ObjDesc->Common.Flags & AOPOBJ_DATA_VALID))
        {
            Status = AcpiDsGetRegionArguments (ObjDesc);
            if (ACPI_FAILURE (Status))
            {
                return_ACPI_STATUS (Status);
            }
        }

        /* Get the table header first so we can get the table length */

        Table = ACPI_ALLOCATE (sizeof (ACPI_TABLE_HEADER));
        if (!Table)
        {
            return_ACPI_STATUS (AE_NO_MEMORY);
        }

        Status = AcpiExRegionRead (ObjDesc, sizeof (ACPI_TABLE_HEADER),
                    ACPI_CAST_PTR (UINT8, Table));
        Length = Table->Length;
        ACPI_FREE (Table);

        if (ACPI_FAILURE (Status))
        {
            return_ACPI_STATUS (Status);
        }

        /* Must have at least an ACPI table header */

        if (Length < sizeof (ACPI_TABLE_HEADER))
        {
            return_ACPI_STATUS (AE_INVALID_TABLE_LENGTH);
        }

        /*
         * The original implementation simply mapped the table, with no copy.
         * However, the memory region is not guaranteed to remain stable and
         * we must copy the table to a local buffer. For example, the memory
         * region is corrupted after suspend on some machines. Dynamically
         * loaded tables are usually small, so this overhead is minimal.
         *
         * The latest implementation (5/2009) does not use a mapping at all.
         * We use the low-level operation region interface to read the table
         * instead of the obvious optimization of using a direct mapping.
         * This maintains a consistent use of operation regions across the
         * entire subsystem. This is important if additional processing must
         * be performed in the (possibly user-installed) operation region
         * handler. For example, AcpiExec and ASLTS depend on this.
         */

        /* Allocate a buffer for the table */

        TableDesc.Pointer = ACPI_ALLOCATE (Length);
        if (!TableDesc.Pointer)
        {
            return_ACPI_STATUS (AE_NO_MEMORY);
        }

        /* Read the entire table */

        Status = AcpiExRegionRead (ObjDesc, Length,
                    ACPI_CAST_PTR (UINT8, TableDesc.Pointer));
        if (ACPI_FAILURE (Status))
        {
            ACPI_FREE (TableDesc.Pointer);
            return_ACPI_STATUS (Status);
        }

        TableDesc.Address = ObjDesc->Region.Address;
        break;


    case ACPI_TYPE_BUFFER: /* Buffer or resolved RegionField */

        ACPI_DEBUG_PRINT ((ACPI_DB_EXEC,
            "Load table from Buffer or Field %p\n", ObjDesc));

        /* Must have at least an ACPI table header */

        if (ObjDesc->Buffer.Length < sizeof (ACPI_TABLE_HEADER))
        {
            return_ACPI_STATUS (AE_INVALID_TABLE_LENGTH);
        }

        /* Get the actual table length from the table header */

        Table = ACPI_CAST_PTR (ACPI_TABLE_HEADER, ObjDesc->Buffer.Pointer);
        Length = Table->Length;

        /* Table cannot extend beyond the buffer */

        if (Length > ObjDesc->Buffer.Length)
        {
            return_ACPI_STATUS (AE_AML_BUFFER_LIMIT);
        }
        if (Length < sizeof (ACPI_TABLE_HEADER))
        {
            return_ACPI_STATUS (AE_INVALID_TABLE_LENGTH);
        }

        /*
         * Copy the table from the buffer because the buffer could be modified
         * or even deleted in the future
         */
        TableDesc.Pointer = ACPI_ALLOCATE (Length);
        if (!TableDesc.Pointer)
        {
            return_ACPI_STATUS (AE_NO_MEMORY);
        }

        ACPI_MEMCPY (TableDesc.Pointer, Table, Length);
        TableDesc.Address = ACPI_TO_INTEGER (TableDesc.Pointer);
        break;


    default:
        return_ACPI_STATUS (AE_AML_OPERAND_TYPE);
    }

    /* Validate table checksum (will not get validated in TbAddTable) */

    Status = AcpiTbVerifyChecksum (TableDesc.Pointer, Length);
    if (ACPI_FAILURE (Status))
    {
        ACPI_FREE (TableDesc.Pointer);
        return_ACPI_STATUS (Status);
    }

    /* Complete the table descriptor */

    TableDesc.Length = Length;
    TableDesc.Flags = ACPI_TABLE_ORIGIN_ALLOCATED;

    /* Install the new table into the local data structures */

    Status = AcpiTbAddTable (&TableDesc, &TableIndex);
    if (ACPI_FAILURE (Status))
    {
        /* Delete allocated table buffer */

        AcpiTbDeleteTable (&TableDesc);
        return_ACPI_STATUS (Status);
    }

    /*
     * Add the table to the namespace.
     *
     * Note: Load the table objects relative to the root of the namespace.
     * This appears to go against the ACPI specification, but we do it for
     * compatibility with other ACPI implementations.
     */
    Status = AcpiExAddTable (TableIndex, AcpiGbl_RootNode, &DdbHandle);
    if (ACPI_FAILURE (Status))
    {
        /* On error, TablePtr was deallocated above */

        return_ACPI_STATUS (Status);
    }

    /* Store the DdbHandle into the Target operand */

    Status = AcpiExStore (DdbHandle, Target, WalkState);
    if (ACPI_FAILURE (Status))
    {
        (void) AcpiExUnloadTable (DdbHandle);

        /* TablePtr was deallocated above */

        AcpiUtRemoveReference (DdbHandle);
        return_ACPI_STATUS (Status);
    }

    ACPI_INFO ((AE_INFO, "Dynamic OEM Table Load:"));
    AcpiTbPrintTableHeader (0, TableDesc.Pointer);

    /* Remove the reference by added by AcpiExStore above */

    AcpiUtRemoveReference (DdbHandle);

    /* Invoke table handler if present */

    if (AcpiGbl_TableHandler)
    {
        (void) AcpiGbl_TableHandler (ACPI_TABLE_EVENT_LOAD, TableDesc.Pointer,
                    AcpiGbl_TableHandlerContext);
    }

    return_ACPI_STATUS (Status);
}


/*******************************************************************************
 *
 * FUNCTION:    AcpiExUnloadTable
 *
 * PARAMETERS:  DdbHandle           - Handle to a previously loaded table
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Unload an ACPI table
 *
 ******************************************************************************/

ACPI_STATUS
AcpiExUnloadTable (
    ACPI_OPERAND_OBJECT     *DdbHandle)
{
    ACPI_STATUS             Status = AE_OK;
    ACPI_OPERAND_OBJECT     *TableDesc = DdbHandle;
    UINT32                  TableIndex;
    ACPI_TABLE_HEADER       *Table;


    ACPI_FUNCTION_TRACE (ExUnloadTable);


    /*
     * Validate the handle
     * Although the handle is partially validated in AcpiExReconfiguration()
     * when it calls AcpiExResolveOperands(), the handle is more completely
     * validated here.
     *
     * Handle must be a valid operand object of type reference. Also, the
     * DdbHandle must still be marked valid (table has not been previously
     * unloaded)
     */
    if ((!DdbHandle) ||
        (ACPI_GET_DESCRIPTOR_TYPE (DdbHandle) != ACPI_DESC_TYPE_OPERAND) ||
        (DdbHandle->Common.Type != ACPI_TYPE_LOCAL_REFERENCE) ||
        (!(DdbHandle->Common.Flags & AOPOBJ_DATA_VALID)))
    {
        return_ACPI_STATUS (AE_BAD_PARAMETER);
    }

    /* Get the table index from the DdbHandle */

    TableIndex = TableDesc->Reference.Value;

    /* Ensure the table is still loaded */

    if (!AcpiTbIsTableLoaded (TableIndex))
    {
        return_ACPI_STATUS (AE_NOT_EXIST);
    }

    /* Invoke table handler if present */

    if (AcpiGbl_TableHandler)
    {
        Status = AcpiGetTableByIndex (TableIndex, &Table);
        if (ACPI_SUCCESS (Status))
        {
            (void) AcpiGbl_TableHandler (ACPI_TABLE_EVENT_UNLOAD, Table,
                        AcpiGbl_TableHandlerContext);
        }
    }

    /* Delete the portion of the namespace owned by this table */

    Status = AcpiTbDeleteNamespaceByOwner (TableIndex);
    if (ACPI_FAILURE (Status))
    {
        return_ACPI_STATUS (Status);
    }

    (void) AcpiTbReleaseOwnerId (TableIndex);
    AcpiTbSetTableLoadedFlag (TableIndex, FALSE);

    /*
     * Invalidate the handle. We do this because the handle may be stored
     * in a named object and may not be actually deleted until much later.
     */
    DdbHandle->Common.Flags &= ~AOPOBJ_DATA_VALID;
    return_ACPI_STATUS (AE_OK);
}
