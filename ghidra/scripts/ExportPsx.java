// Exports function boundaries and decompiled C for a PS1 executable.
// Usage (headless postScript): ExportPsx.java <outdir> [nodecomp]
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import java.io.*;

public class ExportPsx extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        String outDir = args[0];
        boolean decomp = !(args.length > 1 && args[1].equals("nodecomp"));
        String name = currentProgram.getName().replace(".bin", "");
        PrintWriter funcs = new PrintWriter(new FileWriter(outDir + "/" + name + ".funcs.txt"));
        FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
        DecompInterface di = null;
        PrintWriter c = null;
        if (decomp) {
            di = new DecompInterface();
            di.openProgram(currentProgram);
            c = new PrintWriter(new FileWriter(outDir + "/" + name + ".decomp.c"));
        }
        while (it.hasNext() && !monitor.isCancelled()) {
            Function f = it.next();
            StringBuilder sb = new StringBuilder();
            sb.append(String.format("%08x %s", f.getEntryPoint().getOffset(), f.getName()));
            for (AddressRange r : f.getBody()) {
                sb.append(String.format(" %08x-%08x", r.getMinAddress().getOffset(), r.getMaxAddress().getOffset()));
            }
            funcs.println(sb.toString());
            if (decomp) {
                DecompileResults res = di.decompileFunction(f, 30, monitor);
                if (res != null && res.decompileCompleted()) {
                    c.println("// " + String.format("%08x", f.getEntryPoint().getOffset()));
                    c.println(res.getDecompiledFunction().getC());
                } else {
                    c.println(String.format("// %08x: decompile failed", f.getEntryPoint().getOffset()));
                }
            }
        }
        funcs.close();
        if (c != null) c.close();
    }
}
