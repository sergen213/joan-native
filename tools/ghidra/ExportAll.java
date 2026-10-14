// Export decompiled C, function/call table, strings with xrefs, imports with xrefs.
// Usage (headless postScript): ExportAll.java <outDir>
// joan.exe writes <outDir>/joan_decomp.c, functions.tsv, strings.tsv, imports.tsv;
// any other program writes the same set into <outDir>/<programName>/ (decomp file = decomp.c).
//@category JoA

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;
import java.util.concurrent.ConcurrentSkipListMap;

import ghidra.app.decompiler.*;
import ghidra.app.decompiler.parallel.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.util.task.TaskMonitor;

public class ExportAll extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File out = new File(args.length > 0 ? args[0] : "work/ghidra/export");
		String prog = currentProgram.getName();
		String decompName = "decomp.c";
		if (prog.equalsIgnoreCase("joan.exe")) {
			decompName = "joan_decomp.c";
		}
		else {
			out = new File(out, prog);
		}
		out.mkdirs();

		exportFunctions(new File(out, "functions.tsv"));
		exportStrings(new File(out, "strings.tsv"));
		exportImports(new File(out, "imports.tsv"));
		exportDecomp(new File(out, decompName));
		println("ExportAll: wrote " + out.getAbsolutePath());
	}

	private PrintWriter open(File f) throws IOException {
		return new PrintWriter(new BufferedWriter(
			new OutputStreamWriter(new FileOutputStream(f), StandardCharsets.UTF_8)));
	}

	private static String clean(String s) {
		return s.replace("\\", "\\\\").replace("\t", "\\t").replace("\r", "\\r").replace("\n", "\\n");
	}

	private String joinNames(Set<Function> fs) {
		StringBuilder sb = new StringBuilder();
		for (Function f : fs) {
			if (sb.length() > 0) sb.append(';');
			sb.append(f.getName());
		}
		return sb.toString();
	}

	private Set<Function> refFuncs(Address to) {
		Set<Function> s = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
		FunctionManager fm = currentProgram.getFunctionManager();
		for (Reference r : getReferencesTo(to)) {
			Function f = fm.getFunctionContaining(r.getFromAddress());
			if (f != null) s.add(f);
		}
		return s;
	}

	private void exportFunctions(File file) throws Exception {
		try (PrintWriter w = open(file)) {
			w.println("addr\tname\tsize\tncallers\tncallees\tcallers\tcallees");
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				Set<Function> callers = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
				callers.addAll(f.getCallingFunctions(TaskMonitor.DUMMY));
				// joan.exe is incrementally linked: most calls go through ILT `jmp` thunks,
				// so also count the callers of every thunk that targets f.
				Address[] thunks = f.getFunctionThunkAddresses(true);
				if (thunks != null) {
					FunctionManager fm = currentProgram.getFunctionManager();
					for (Address t : thunks) {
						Function tf = fm.getFunctionAt(t);
						if (tf != null) callers.addAll(tf.getCallingFunctions(TaskMonitor.DUMMY));
					}
				}
				Set<Function> callees = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
				callees.addAll(f.getCalledFunctions(TaskMonitor.DUMMY));
				w.println(f.getEntryPoint() + "\t" + clean(f.getName(true)) + "\t" +
					f.getBody().getNumAddresses() + "\t" + callers.size() + "\t" + callees.size() +
					"\t" + joinNames(callers) + "\t" + joinNames(callees));
			}
		}
	}

	private void exportStrings(File file) throws Exception {
		try (PrintWriter w = open(file)) {
			w.println("addr\ttype\tstring\txref_funcs");
			for (Data d : currentProgram.getListing().getDefinedData(true)) {
				if (!d.hasStringValue()) continue;
				Object v = d.getValue();
				if (v == null) continue;
				w.println(d.getAddress() + "\t" + d.getDataType().getName() + "\t" +
					clean(v.toString()) + "\t" + joinNames(refFuncs(d.getAddress())));
			}
		}
	}

	private void exportImports(File file) throws Exception {
		try (PrintWriter w = open(file)) {
			w.println("library\timport\txref_funcs");
			FunctionManager fm = currentProgram.getFunctionManager();
			for (Function ext : fm.getExternalFunctions()) {
				Set<Function> users = new TreeSet<>(Comparator.comparing(Function::getEntryPoint));
				// direct refs to the external symbol, refs to IAT slots, and calls through thunks
				for (Reference r : getReferencesTo(ext.getEntryPoint())) {
					Function f = fm.getFunctionContaining(r.getFromAddress());
					if (f != null) users.add(f);
					users.addAll(refFuncs(r.getFromAddress()));
				}
				Address[] thunks = ext.getFunctionThunkAddresses(true);
				if (thunks != null) {
					for (Address t : thunks) users.addAll(refFuncs(t));
				}
				String lib = ext.getExternalLocation() != null
						? ext.getExternalLocation().getLibraryName()
						: "";
				w.println(lib + "\t" + ext.getName() + "\t" + joinNames(users));
			}
		}
	}

	private void exportDecomp(File file) throws Exception {
		ConcurrentSkipListMap<Address, String> texts = new ConcurrentSkipListMap<>();
		DecompileConfigurer cfg = d -> {
			DecompileOptions o = new DecompileOptions();
			d.setOptions(o);
			d.toggleCCode(true);
			d.toggleSyntaxTree(false);
			d.setSimplificationStyle("decompile");
		};
		DecompilerCallback<Void> cb = new DecompilerCallback<>(currentProgram, cfg) {
			@Override
			public Void process(DecompileResults r, TaskMonitor m) {
				Function f = r.getFunction();
				DecompiledFunction df = r.getDecompiledFunction();
				String body = df != null ? df.getC()
						: "/* decompile failed: " + r.getErrorMessage() + " */\n";
				texts.put(f.getEntryPoint(),
					"// FUNC " + f.getEntryPoint() + " " + f.getName(true) + "\n" + body + "\n");
				return null;
			}
		};
		cb.setTimeout(120);
		List<Function> fs = new ArrayList<>();
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (!f.isExternal()) fs.add(f);
		}
		try {
			ParallelDecompiler.decompileFunctions(cb, fs, monitor);
		}
		finally {
			cb.dispose();
		}
		try (PrintWriter w = open(file)) {
			for (String s : texts.values()) w.print(s);
		}
		println("ExportAll: decompiled " + texts.size() + "/" + fs.size());
	}
}
