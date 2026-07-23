// Applies only manually verified game symbols. Descriptive names are used when
// an original source-level name is not available.
// @category SyphonFilterPC

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.SourceType;

public class ApplyKnownSymbols extends GhidraScript {
    private void rename(long rawAddress, String name) throws Exception {
        Address address = toAddr(rawAddress);
        Function function = currentProgram.getFunctionManager().getFunctionAt(address);
        if (function == null) {
            throw new IllegalStateException("No analyzed function at " + address);
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
        rename(0x800145b4L, "System_RunStateMachine");
        rename(0x80015e80L, "System_SetState");
        rename(0x80016020L, "System_PushState");
        rename(0x80016094L, "System_PopState");
        rename(0x800d7758L, "SetVideoMode");
    }
}
