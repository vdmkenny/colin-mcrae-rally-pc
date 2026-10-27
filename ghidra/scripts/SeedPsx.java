// Seeds function starts in a raw PS1 executable before auto-analysis:
// the EXE entry point, every jal target inside the image and every
// "addiu sp,sp,-N" that directly follows a "jr ra" + delay slot.
// Usage (headless preScript): SeedPsx.java <entry-hex>
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.*;
import ghidra.program.model.mem.*;
import java.util.*;

public class SeedPsx extends GhidraScript {
    @Override
    public void run() throws Exception {
        long entry = Long.parseLong(getScriptArgs()[0], 16);
        Memory mem = currentProgram.getMemory();
        MemoryBlock blk = mem.getBlocks()[0];
        long lo = blk.getStart().getOffset(), hi = blk.getEnd().getOffset() + 1;
        AddressSpace sp = currentProgram.getAddressFactory().getDefaultAddressSpace();
        TreeSet<Long> seeds = new TreeSet<>();
        seeds.add(entry);
        int prev2 = 0, prev1 = 0;
        for (long a = lo; a + 4 <= hi; a += 4) {
            int w = mem.getInt(sp.getAddress(a));
            if ((w >>> 26) == 3) {
                long t = (a & 0xf0000000L) | ((long)(w & 0x03ffffff) << 2);
                if (t >= lo && t < hi) seeds.add(t);
            }
            if ((w & 0xffff8000) == 0x27bd8000 && prev2 == 0x03e00008) seeds.add(a);
            prev2 = prev1; prev1 = w;
        }
        for (long s : seeds) {
            Address ad = sp.getAddress(s);
            new DisassembleCommand(ad, null, true).applyTo(currentProgram, monitor);
        }
        for (long s : seeds) {
            Address ad = sp.getAddress(s);
            if (getFunctionAt(ad) == null) new CreateFunctionCmd(ad).applyTo(currentProgram, monitor);
        }
        println("seeded " + seeds.size() + " functions");
    }
}
