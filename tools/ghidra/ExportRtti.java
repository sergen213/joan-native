// Dump MSVC RTTI type descriptors (".?AV"/".?AU" names) and Ghidra-recovered vftables.
// Output: <outDir>/rtti.tsv (joan.exe) or <outDir>/<program>/rtti.tsv.
//   typedesc  mangled  class(namespace)  vftables(addr:slots;...)
//@category JoA

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.symbol.Symbol;

public class ExportRtti extends GhidraScript {
	@Override
	public void run() throws Exception {
		File out = new File(getScriptArgs()[0]);
		if (!currentProgram.getName().equalsIgnoreCase("joan.exe")) {
			out = new File(out, currentProgram.getName());
		}
		out.mkdirs();

		// namespace -> vftables
		Map<String, List<String>> vt = new TreeMap<>();
		for (Symbol s : currentProgram.getSymbolTable().getAllSymbols(true)) {
			if (!s.getName().equals("vftable") && !s.getName().startsWith("vftable_")) continue;
			vt.computeIfAbsent(s.getParentNamespace().getName(true), k -> new ArrayList<>())
					.add(s.getAddress() + ":" + slots(s.getAddress()));
		}

		Memory mem = currentProgram.getMemory();
		byte[] pat = ".?A".getBytes(StandardCharsets.US_ASCII);
		try (PrintWriter w = new PrintWriter(new OutputStreamWriter(
			new FileOutputStream(new File(out, "rtti.tsv")), StandardCharsets.UTF_8))) {
			w.println("typedesc\tmangled\tclass\tvftables");
			Address a = mem.getMinAddress();
			Set<String> seen = new HashSet<>();
			while (a != null) {
				a = mem.findBytes(a, pat, null, true, monitor);
				if (a == null) break;
				StringBuilder sb = new StringBuilder();
				Address p = a;
				for (int i = 0; i < 512; i++, p = p.add(1)) {
					byte b = mem.getByte(p);
					if (b == 0) break;
					sb.append((char) (b & 0xff));
				}
				String m = sb.toString();
				String cls = demangle(m);
				seen.add(cls);
				List<String> v = vt.getOrDefault(cls, List.of());
				w.println(a.subtract(8) + "\t" + m + "\t" + cls + "\t" + String.join(";", v));
				a = p;
			}
			// vftables whose namespace had no matching type-descriptor string
			for (var e : vt.entrySet()) {
				if (!seen.contains(e.getKey())) {
					w.println("-\t-\t" + e.getKey() + "\t" + String.join(";", e.getValue()));
				}
			}
		}
	}

	// ".?AVFoo@Bar@@" -> "Bar::Foo" (templates/odd names returned as-is minus prefix)
	private static String demangle(String m) {
		String s = m.substring(4);
		if (s.endsWith("@@")) s = s.substring(0, s.length() - 2);
		if (s.contains("?") || s.contains("$")) return s;
		List<String> parts = new ArrayList<>(Arrays.asList(s.split("@")));
		Collections.reverse(parts);
		return String.join("::", parts);
	}

	private int slots(Address a) {
		int n = 0;
		try {
			while (n < 1024) {
				Address t = toAddr(getInt(a.add(4L * n)) & 0xffffffffL);
				if (getFunctionAt(t) == null && getInstructionAt(t) == null) break;
				if (n > 0 && getSymbolAt(a.add(4L * n)) != null) break;
				n++;
			}
		}
		catch (Exception e) {
			// ran off memory
		}
		return n;
	}
}
