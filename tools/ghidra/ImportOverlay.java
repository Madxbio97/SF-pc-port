// Imports one raw PSX overlay into a named Ghidra overlay address space and
// seeds a verified entry function.
// @category SyphonFilterPC

import java.io.File;
import java.nio.file.Files;
import java.util.Arrays;

import ghidra.app.cmd.memory.AddInitializedMemoryBlockCmd;
import ghidra.app.script.GhidraScript;
import ghidra.app.util.importer.MessageLog;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;
import psx.PsxLoader;

public class ImportOverlay extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] arguments = getScriptArgs();
        if (arguments.length != 5) {
            throw new IllegalArgumentException(
                "Usage: ImportOverlay.java <name> <file> <base> <entry> <entry-name>");
        }

        String blockName = arguments[0];
        File input = new File(arguments[1]);
        long base = Long.decode(arguments[2]);
        long entryOffset = Long.decode(arguments[3]) - base;
        String entryName = arguments[4];
        byte[] fileData = Files.readAllBytes(input.toPath());
        if (entryOffset < 0 || entryOffset >= fileData.length) {
            throw new IllegalArgumentException("Overlay entry is outside the input file");
        }

        Memory memory = currentProgram.getMemory();
        MemoryBlock block = memory.getBlock(blockName);
        if (block == null) {
            AddressSpace defaultSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();
            Address start = defaultSpace.getAddressInThisSpaceOnly(base);
            AddInitializedMemoryBlockCmd command = new AddInitializedMemoryBlockCmd(
                blockName,
                null,
                input.getAbsolutePath(),
                start,
                fileData.length,
                true,
                true,
                true,
                false,
                (byte) 0,
                true);
            if (!command.applyTo(currentProgram)) {
                throw new IllegalStateException(command.getStatusMsg());
            }
            block = memory.getBlock(blockName);
            memory.setBytes(block.getStart(), fileData);

            long gp = PsxLoader.getGpBase(currentProgram);
            PsxLoader.setRegisterValue(
                currentProgram,
                "gp",
                block.getStart(),
                block.getEnd(),
                gp,
                new MessageLog());
            println("Created overlay " + blockName + " at " + block.getStart());
        } else {
            if (!block.isOverlay() || block.getSize() != fileData.length ||
                block.getStart().getOffset() != base) {
                throw new IllegalStateException("Existing overlay metadata does not match " + blockName);
            }
            byte[] existing = new byte[fileData.length];
            memory.getBytes(block.getStart(), existing);
            if (!Arrays.equals(existing, fileData)) {
                throw new IllegalStateException("Existing overlay bytes do not match " + input);
            }
            println("Verified existing overlay " + blockName);
        }

        Address entry = block.getStart().add(entryOffset);
        disassemble(entry);
        Function function = getFunctionAt(entry);
        if (function == null) {
            function = createFunction(entry, entryName);
        } else if (function.getName().startsWith("FUN_") || function.getName().equals(entryName)) {
            function.setName(entryName, SourceType.USER_DEFINED);
        }
        if (function == null) {
            throw new IllegalStateException("Failed to create overlay entry function at " + entry);
        }
        analyzeChanges(currentProgram);
        println(entry + " -> " + function.getName(true));
    }
}
