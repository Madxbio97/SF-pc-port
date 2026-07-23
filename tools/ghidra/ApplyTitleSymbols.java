// Applies descriptive names to TITLE.OVL functions recovered from control flow.
// These are clean-room names, not claimed original source identifiers.
// @category SyphonFilterPC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyTitleSymbols extends GhidraScript {
    private void rename(String rawAddress, String name) throws Exception {
        Address address = toAddr("TITLE_OVL::" + rawAddress);
        Function function = currentProgram.getFunctionManager().getFunctionAt(address);
        if (function == null) {
            disassemble(address);
            function = createFunction(address, null);
        }
        if (function == null) {
            throw new IllegalStateException("Unable to create TITLE.OVL function at " + address);
        }
        String currentName = function.getName();
        if (currentName.startsWith("FUN_") || currentName.equals(name)) {
            function.setName(name, SourceType.USER_DEFINED);
            println(address + " -> " + name);
        } else {
            println(address + " kept existing symbol " + currentName);
        }
    }

    @Override
    public void run() throws Exception {
        rename("801473a4", "Title_SetViewportTarget");
        rename("8014748c", "Title_UpdateViewport");
        rename("8014775c", "Title_PushMenu");
        rename("8014785c", "Title_PopMenu");
        rename("80147924", "Title_Initialize");
        rename("80147a70", "Title_Shutdown");
        rename("80147b0c", "Title_QueueOperation");
        rename("80147b90", "Title_DequeueOperation");
        rename("80147c08", "Title_ResetOperations");
        rename("80147c34", "Title_OpenDialog");
        rename("80147f28", "Title_SaveGame");
        rename("80148070", "Title_DispatchOperation");
        rename("80148230", "Title_ProcessSaveOperation");
        rename("80148474", "Title_LoadMenuSprites");
        rename("80148610", "Title_FocusTextItem");
        rename("801486a8", "Title_DetachMenuSprites");
        rename("80148740", "Title_ConfirmNewGame");
        rename("80148770", "Title_AcceptMainMenuSelection");
        rename("801489d8", "Title_AcceptDialogSelection");
        rename("80148a54", "Title_AcceptSaveMenuSelection");
        rename("80148b1c", "Title_AdvanceLogoSequence");
        rename("80148c00", "Title_AcceptLoadMenuSelection");
        rename("80148cb4", "Title_CloseTrainingMovie");
        rename("80148cd4", "Title_SetSelection");
        rename("80148d88", "Title_SetTextMenuSelection");
        rename("80148e70", "Title_SetLoadMenuSelection");
        rename("80148f78", "Title_StartTitleMovie");
        rename("80149048", "Title_UpdateTextMenuColors");
        rename("80149154", "Title_StartLogoSequence");
        rename("801491b8", "Title_UpdateLoadMenu");
        rename("80149300", "Title_StartTrainingMovie");
        rename("80149328", "Title_UpdateMenuSprites");
        rename("80149558", "Title_RebuildMenu");
        rename("80149830", "Title_AcceptSelection");
        rename("80149880", "Title_HandleInput");
        rename("80149bec", "Title_BuildLoadMenu");
        rename("80149cf4", "Title_Update");
    }
}
