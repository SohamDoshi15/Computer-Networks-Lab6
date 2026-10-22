#!/usr/bin/env python3
"""Generate the consolidated IT305 final report from repository evidence."""

from __future__ import annotations

import csv
import os
from collections import defaultdict
from pathlib import Path
from statistics import mean, pstdev
from xml.sax.saxutils import escape

from reportlab.lib import colors
from reportlab.lib.enums import TA_CENTER, TA_LEFT
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle, getSampleStyleSheet
from reportlab.lib.units import cm
from reportlab.platypus import (
    BaseDocTemplate,
    Frame,
    HRFlowable,
    Image,
    KeepTogether,
    PageBreak,
    PageTemplate,
    Paragraph,
    Spacer,
    Table,
    TableStyle,
)


ROOT = Path(__file__).resolve().parents[1]
RESULTS = ROOT / "Results"
OUTPUT = ROOT / "output" / "pdf" / "FinalReport_Group_064_091.pdf"

NAVY = colors.HexColor("#102A43")
BLUE = colors.HexColor("#176B87")
TEAL = colors.HexColor("#1B998B")
AMBER = colors.HexColor("#D97706")
PALE_BLUE = colors.HexColor("#EAF4F8")
PALE_AMBER = colors.HexColor("#FFF4DB")
LIGHT = colors.HexColor("#F5F7FA")
MID = colors.HexColor("#627D98")
INK = colors.HexColor("#243B53")


def read_csv(name: str) -> list[dict[str, str]]:
    path = RESULTS / name
    with path.open(newline="", encoding="utf-8") as handle:
        return list(csv.DictReader(handle))


def grouped_means(rows, keys, metric, predicate=lambda _: True):
    groups = defaultdict(list)
    for row in rows:
        if predicate(row):
            groups[tuple(row[key] for key in keys)].append(float(row[metric]))
    return {key: (mean(vals), pstdev(vals), len(vals)) for key, vals in groups.items()}


def table(data, widths=None, header=True, font_size=8.2):
    header_style = ParagraphStyle(
        "TableHeader", fontName="Helvetica-Bold", fontSize=font_size,
        leading=font_size + 2.0, textColor=colors.white,
    )
    body_style = ParagraphStyle(
        "TableBody", fontName="Helvetica", fontSize=font_size,
        leading=font_size + 2.2, textColor=INK,
    )
    wrapped = []
    for row_index, row in enumerate(data):
        cell_style = header_style if header and row_index == 0 else body_style
        wrapped.append([Paragraph(escape(str(value)), cell_style) for value in row])
    result = Table(wrapped, colWidths=widths, repeatRows=1 if header else 0, hAlign="LEFT")
    commands = [
        ("FONTNAME", (0, 0), (-1, -1), "Helvetica"),
        ("FONTSIZE", (0, 0), (-1, -1), font_size),
        ("LEADING", (0, 0), (-1, -1), font_size + 2.2),
        ("TEXTCOLOR", (0, 0), (-1, -1), INK),
        ("GRID", (0, 0), (-1, -1), 0.35, colors.HexColor("#BCCCDC")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 5),
        ("RIGHTPADDING", (0, 0), (-1, -1), 5),
        ("TOPPADDING", (0, 0), (-1, -1), 4),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4),
    ]
    if header:
        commands += [
            ("BACKGROUND", (0, 0), (-1, 0), NAVY),
            ("TEXTCOLOR", (0, 0), (-1, 0), colors.white),
            ("FONTNAME", (0, 0), (-1, 0), "Helvetica-Bold"),
        ]
        if len(data) > 1:
            commands.append(("ROWBACKGROUNDS", (0, 1), (-1, -1), [colors.white, LIGHT]))
    result.setStyle(TableStyle(commands))
    return result


def report_image(name: str, width=16.6 * cm):
    path = RESULTS / name
    if not path.exists():
        raise FileNotFoundError(path)
    image = Image(str(path))
    image._restrictSize(width, 10.8 * cm)
    image.hAlign = "CENTER"
    return image


def paragraph(text, style):
    return Paragraph(text, style)


def build_report() -> None:
    part1 = grouped_means(read_csv("Part1_runs.csv"), ("server_mode", "clients"), "aggregate_throughput_MBps")
    case1_rows = read_csv("Part2_Case1_runs.csv")
    case2_rows = read_csv("Part2_Case2_runs.csv")
    enhanced_rows = read_csv("Part2_Case2_Enhanced_runs.csv")
    case1 = grouped_means(
        case1_rows, ("failure_probability",), "goodput_MBps",
        lambda r: r["fault_mode"] == "connection" and r["status"] == "0",
    )
    case2 = grouped_means(
        case2_rows, ("failure_probability",), "goodput_MBps",
        lambda r: r["fault_mode"] == "connection" and r["status"] == "0",
    )
    enhanced = grouped_means(
        enhanced_rows, ("parallel_N", "failure_probability"), "throughput_mbps"
    )

    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    styles = getSampleStyleSheet()
    styles.add(ParagraphStyle(
        name="ReportTitle", parent=styles["Title"], fontName="Helvetica-Bold",
        fontSize=25, leading=29, textColor=NAVY, alignment=TA_CENTER, spaceAfter=12,
    ))
    styles.add(ParagraphStyle(
        name="Subtitle", parent=styles["Normal"], fontName="Helvetica",
        fontSize=13, leading=18, textColor=BLUE, alignment=TA_CENTER,
    ))
    styles.add(ParagraphStyle(
        name="H1x", parent=styles["Heading1"], fontName="Helvetica-Bold",
        fontSize=17, leading=21, textColor=NAVY, spaceBefore=4, spaceAfter=9,
    ))
    styles.add(ParagraphStyle(
        name="H2x", parent=styles["Heading2"], fontName="Helvetica-Bold",
        fontSize=12.5, leading=16, textColor=BLUE, spaceBefore=10, spaceAfter=6,
    ))
    styles.add(ParagraphStyle(
        name="Bodyx", parent=styles["BodyText"], fontName="Helvetica",
        fontSize=9.4, leading=13.5, textColor=INK, spaceAfter=7,
    ))
    styles.add(ParagraphStyle(
        name="Smallx", parent=styles["BodyText"], fontName="Helvetica",
        fontSize=8, leading=11, textColor=MID, spaceAfter=5,
    ))
    styles.add(ParagraphStyle(
        name="Callout", parent=styles["BodyText"], fontName="Helvetica-Bold",
        fontSize=9.2, leading=13, textColor=NAVY, borderColor=AMBER,
        borderWidth=1, borderPadding=9, backColor=PALE_AMBER, spaceBefore=5, spaceAfter=10,
    ))
    styles.add(ParagraphStyle(
        name="Codex", parent=styles["Code"], fontName="Courier", fontSize=7.7,
        leading=10.5, textColor=colors.HexColor("#102A43"), backColor=LIGHT,
        borderPadding=7, spaceBefore=4, spaceAfter=9,
    ))

    body = Frame(1.75 * cm, 1.65 * cm, A4[0] - 3.5 * cm, A4[1] - 3.25 * cm, id="body")

    def decorate(canvas, doc):
        canvas.saveState()
        canvas.setStrokeColor(colors.HexColor("#D9E2EC"))
        canvas.line(1.75 * cm, 1.25 * cm, A4[0] - 1.75 * cm, 1.25 * cm)
        canvas.setFont("Helvetica", 7.5)
        canvas.setFillColor(MID)
        canvas.drawString(1.75 * cm, 0.83 * cm, "IT305 Lab | Groups 064 and 091")
        canvas.drawRightString(A4[0] - 1.75 * cm, 0.83 * cm, f"Page {doc.page}")
        if doc.page > 1:
            canvas.setFillColor(NAVY)
            canvas.setFont("Helvetica-Bold", 7.5)
            canvas.drawString(1.75 * cm, A4[1] - 1.05 * cm, "Fault-Tolerant Topic-Based File Distribution Service")
        canvas.restoreState()

    doc = BaseDocTemplate(
        str(OUTPUT), pagesize=A4, leftMargin=1.75 * cm, rightMargin=1.75 * cm,
        topMargin=1.6 * cm, bottomMargin=1.65 * cm,
        title="IT305 Final Report - Groups 064 and 091",
        author="Purva Kaneria, Soham Doshi, Shaurya Shekhawat, Om Patel, Aditya Vaish, Tirth Patel",
    )
    doc.addPageTemplates(PageTemplate(id="main", frames=[body], onPage=decorate))

    S = []
    S += [Spacer(1, 0.7 * cm), paragraph("IT305 LAB", styles["Subtitle"]), Spacer(1, 0.25 * cm)]
    S += [paragraph("Fault-Tolerant Topic-Based<br/>File Distribution Service", styles["ReportTitle"])]
    S += [paragraph("Final Code and Result Analysis", styles["Subtitle"]), Spacer(1, 0.3 * cm)]
    S += [HRFlowable(width="70%", thickness=2, color=TEAL, hAlign="CENTER"), Spacer(1, 0.35 * cm)]
    S += [paragraph("Groups 064 and 091", styles["Subtitle"]), Spacer(1, 0.4 * cm)]
    contributions = [
        ["Member", "Roll number", "Individual contribution"],
        ["Purva Kaneria", "20241171", "Enhanced Case 2 parallel ranges, experiment harness, plots and analysis."],
        ["Soham Doshi", "202401208", "Part I topic discovery, safe paths, sequential and multi-threaded servers."],
        ["Shaurya Shekhawat", "202401197", "Case 2 session store, durable checkpoints and resume protocol."],
        ["Om Patel", "202401439", "Shared protocol, integration, documentation, hardening and final-package checks."],
        ["Aditya Vaish", "202401401", "Common client, safe file publication, integrity checks and client CLI."],
        ["Tirth Patel", "202401227", "Case 1 fault injection, restart client, metrics and experiment workflow."],
    ]
    S += [table(contributions, [3.0 * cm, 2.2 * cm, 11.2 * cm], font_size=7.2), Spacer(1, 0.45 * cm)]
    S += [paragraph("Report generated from repository state and measured validation runs on 22 October 2026.", styles["Smallx"])]
    S += [PageBreak()]

    S += [paragraph("Executive Summary", styles["H1x"])]
    S += [paragraph(
        "The project implements a C11/POSIX TCP service that maps topic names to directories and transfers every regular file in a requested topic. Part I compares sequential and thread-per-client servers. Part II adds probabilistic disconnection, restart-only recovery, durable checkpoint resumption, and parallel byte-range transfer. The implementation uses a shared 28-byte frame header, bounded payloads, explicit offsets, SHA-256 integrity checks, safe temporary files, and strict command-line validation.",
        styles["Bodyx"],
    )]
    S += [paragraph(
        "Current code status: all programs compile with GCC using -Wall -Wextra -Wpedantic -Werror; five unit suites and four TCP end-to-end suites pass; AddressSanitizer and UndefinedBehaviorSanitizer pass across unit tests and every end-to-end case. The generated submission layout also builds each required case independently.",
        styles["Bodyx"],
    )]
    S += [paragraph(
        "Evidence boundary: the performance figures in this report are real same-host WSL2 loopback measurements on a 12.58 MB validation workload. They verify the instrumentation and relative behavior, but they are not the assignment's required Animals-10 experiment using one server computer and two client computers. That final physical experiment remains the only unresolved submission requirement.",
        styles["Callout"],
    )]
    S += [paragraph("The cover records each member's primary contribution. Repository history and module ownership support those lines; every member remains jointly responsible for the whole system and viva.", styles["Smallx"])]

    S += [PageBreak(), paragraph("1. Requirements and Compliance", styles["H1x"])]
    compliance = [
        ["Requirement", "Implemented evidence", "Status"],
        ["Part I sequential and concurrent servers", "server_single and server_multi share validated transfer logic", "Pass"],
        ["Required CLI; no fixed path/IP/port", "Strict positional parsing; dataset root, port, host and output are runtime arguments", "Pass"],
        ["Case 1 restart after disconnect", "Fresh GET and transfer ID; failed attempt output discarded", "Pass"],
        ["Case 2 checkpoint resume", "Atomic client checkpoint plus validated in-memory server session", "Pass"],
        ["Enhanced parallel connections", "N workers request non-overlapping ranges and reassemble with pwrite", "Pass"],
        ["Per-case Makefile builds server/client", "Generated Part1 and three Part2 folders build exactly two executables", "Pass"],
        ["Raw logs and graphs for all cases", "CSV and PNG outputs exist and are reproducible", "Local validation"],
        ["Three-computer Animals-10 experiment", "Procedure is documented; current repository does not contain those final measurements", "Pending"],
    ]
    S += [table(compliance, [4.6 * cm, 9.6 * cm, 2.1 * cm], font_size=7.7)]
    S += [paragraph("Required interfaces", styles["H2x"])]
    S += [paragraph(
        "Part I: ./server &lt;port&gt; &lt;topics_root_dir&gt;<br/>"
        "Part II: ./server &lt;port&gt; &lt;topics_root_dir&gt; &lt;failure_probability&gt;<br/>"
        "All clients: ./client &lt;server_ip&gt; &lt;server_port&gt; &lt;topic&gt; &lt;output_dir&gt;",
        styles["Codex"],
    )]
    S += [paragraph("The enhanced client accepts an optional trailing parallel-connection count while preserving the required four-argument client invocation.", styles["Bodyx"])]

    S += [paragraph("2. Architecture and Wire Protocol", styles["H1x"])]
    architecture = [
        ["Layer", "Responsibility"],
        ["common/", "Frame encoding, network-order integers, full-buffer send/receive, CLI parsing, Case 2 and range payload codecs."],
        ["server/", "Topic manifest construction, safe file access, sequential/concurrent service, injected faults, session state and range workers."],
        ["client/", "Manifest validation, temporary-file writes, SHA-256 publication, restart/resume clients and parallel reassembly."],
        ["tests/", "Unit boundaries, malformed peers, real TCP transfers, disconnects, restart fallback, modified-manifest fallback and concurrency."],
        ["scripts/", "Repeatable experiments, summaries, plotting and creation of the graded directory layout."],
    ]
    S += [table(architecture, [3.0 * cm, 13.2 * cm], font_size=8)]
    S += [paragraph("Frame format", styles["H2x"])]
    S += [paragraph(
        "Every message begins with version (1 byte), type (1), flags (2), payload length (8, network order), and transfer ID (16): 28 bytes total. Receivers loop until the full header and declared payload arrive. Payload limits are enforced before allocation, and every DATA frame carries a file index and absolute offset.",
        styles["Bodyx"],
    )]
    message_rows = [
        ["Phase", "Messages"],
        ["Base transfer", "GET, MANIFEST, FILE_START, DATA, FILE_END, ACK, COMPLETE, ERROR"],
        ["Checkpoint resume", "RESUME, RESUME_OK plus base transfer messages"],
        ["Parallel ranges", "ASSIGN_RANGE, RANGE_READY, DATA, ACK_RANGE, RANGE_END"],
    ]
    S += [table(message_rows, [4.0 * cm, 12.2 * cm], font_size=8.2)]
    S += [KeepTogether([
        paragraph("Security and correctness controls", styles["H2x"]),
        paragraph(
            "Topic and file names reject traversal, separators, absolute components, control characters and duplicates. The server serves only regular files beneath the selected topic. The client writes .part files, flushes them, verifies exact size and SHA-256, then atomically renames. Socket I/O uses deadlines and treats short frames as interruption. Manifest identities prevent resuming against changed server content.",
            styles["Bodyx"],
        ),
    ])]

    S += [paragraph("3. Part I - Basic Topic Distribution", styles["H1x"])]
    S += [paragraph(
        "The single-threaded server accepts one client and completes its request before returning to accept. The multi-threaded server allocates a per-client context and detaches one worker thread for every accepted connection. Both modes use the same sorted manifest, framing and file-transfer implementation, so the comparison isolates scheduling rather than protocol differences.",
        styles["Bodyx"],
    )]
    p1_table = [["Mode", "Clients", "Mean MB/s", "Std. dev."]]
    for mode in ("single", "multi"):
        for clients in ("1", "2", "4"):
            avg, dev, _ = part1[(mode, clients)]
            p1_table.append([mode.title(), clients, f"{avg:.2f}", f"{dev:.2f}"])
    S += [table(p1_table, [4.0 * cm, 3.2 * cm, 4.0 * cm, 4.0 * cm])]
    S += [Spacer(1, 0.25 * cm), report_image("Part1_throughput.png")]
    S += [paragraph(
        "In loopback validation, sequential throughput stayed near 24-26 MB/s as client count increased because requests were serialized. The multi-threaded server rose from 25.47 MB/s at one client to 80.74 MB/s at four clients, a 3.17x aggregate increase. This is a host-local concurrency result, not a lab-network capacity claim.",
        styles["Bodyx"],
    )]

    S += [PageBreak(), paragraph("4. Part II Case 1 - Restart Without Session State", styles["H1x"])]
    S += [paragraph(
        "After a manifest is sent, the server either completes normally or cuts the stream at a seeded byte position selected with probability p. A cut can use FIN or TCP RST. The client retains no progress: it removes partial output, reconnects, sends a new GET with a new transfer ID, and restarts from byte zero. Exact counters distinguish useful, redundant and total transmitted bytes.",
        styles["Bodyx"],
    )]
    c1_table = [["Failure p", "Mean goodput MB/s", "Std. dev.", "Runs"]]
    for p in ("0", "0.2", "0.5"):
        avg, dev, count = case1[(p,)]
        c1_table.append([p, f"{avg:.2f}", f"{dev:.2f}", str(count)])
    S += [table(c1_table, [4.0 * cm] * 4)]
    S += [Spacer(1, 0.2 * cm), report_image("Part2_Case1_overhead.png")]
    S += [paragraph(
        "The p=0.5 mean goodput is noisy because two of three seeded runs had no cut. The byte metric remains diagnostic: mean redundant data rose to 2.18 MB. Theory predicts geometric attempts and expected redundant data pD/(2(1-p)); more repetitions are needed for stable high-p timing.",
        styles["Bodyx"],
    )]

    S += [PageBreak(), paragraph("5. Part II Case 2 - Checkpointed Resume", styles["H1x"])]
    S += [paragraph(
        "The client creates a 16-byte transfer ID and persists topic, manifest identity, current file, contiguous offset and completed-file digests to an atomic checkpoint. The server stores only acknowledged progress in a mutex-protected session table. On reconnect, RESUME is accepted only when transfer ID, topic, manifest digest, completed hashes and requested offset agree with server state. Missing or stale server state safely falls back to a clean GET.",
        styles["Bodyx"],
    )]
    c2_table = [["Failure p", "Mean goodput MB/s", "Std. dev.", "Runs"]]
    for p in ("0", "0.2", "0.5"):
        avg, dev, count = case2[(p,)]
        c2_table.append([p, f"{avg:.2f}", f"{dev:.2f}", str(count)])
    S += [table(c2_table, [4.0 * cm] * 4)]
    S += [Spacer(1, 0.25 * cm), report_image("Part2_Case2_overhead.png")]
    S += [paragraph(
        "Mean redundant data increased with p but remained bounded by acknowledged progress rather than requiring every completed file to be retransmitted. The validation environment makes durable fsync checkpoints expensive, so Case 2 goodput is lower than Case 1 in these short loopback runs. This is a real implementation tradeoff: durability reduces re-sent bytes but adds synchronous storage work.",
        styles["Bodyx"],
    )]

    S += [PageBreak(), paragraph("6. Part II Case 2 Enhanced - Parallel Ranges", styles["H1x"])]
    S += [paragraph(
        "The enhanced client obtains the manifest, partitions files into disjoint half-open ranges, preallocates temporary files, and schedules ranges across N worker connections. Workers write with pwrite at explicit offsets, so independent ranges do not race on a shared file position. When a connection drops, only that range reconnects from its current offset; completed ranges and other workers continue.",
        styles["Bodyx"],
    )]
    enhanced_table = [["p", "N=1 MB/s", "N=2 MB/s", "N=4 MB/s"]]
    for p in ("0", "0.2", "0.5"):
        enhanced_table.append([p] + [f"{enhanced[(n, p)][0]:.2f}" for n in ("1", "2", "4")])
    S += [table(enhanced_table, [4.0 * cm] * 4)]
    S += [Spacer(1, 0.2 * cm), report_image("Part2_Case2_Enhanced_throughput.png")]
    S += [paragraph(
        "At p=0, N=4 reached 6.86 MB/s versus 2.39 MB/s for N=1 (2.88x). At p=0.2, the measured speedup was 2.92x. Throughput declined with faults, but unaffected workers continued. No complete DATA payload was received twice in this measured sweep, so redundant payload bytes were zero while control overhead increased slightly.",
        styles["Bodyx"],
    )]

    S += [PageBreak(), paragraph("7. Cross-Case Analysis", styles["H1x"])]
    S += [report_image("Case_Comparison_Throughput.png")]
    S += [paragraph(
        "All three Part II curves above use the same 12.58 MB workload and failure points. Case 1 is fastest in this local environment because it avoids durable checkpoint fsync cost and most three-run seeds completed without interruption. Case 2 trades throughput for durable, validated progress. Enhanced Case 2 recovers much of that cost through parallelism. The graph must not be generalized beyond this workload or used as the required three-computer result.",
        styles["Bodyx"],
    )]
    S += [paragraph("Metrics", styles["H2x"])]
    metric_rows = [
        ["Metric", "Definition"],
        ["Useful bytes", "Unique file bytes in the final verified output tree."],
        ["Transmitted bytes", "All server-to-client bytes observed across every attempt."],
        ["Redundant bytes", "Complete DATA payload bytes received beyond useful bytes."],
        ["Wasted bytes", "All bytes received on failed attempts."],
        ["Goodput", "Useful bytes divided by end-to-end completion time."],
        ["Overhead", "(transmitted bytes - useful bytes) / useful bytes."],
    ]
    S += [table(metric_rows, [4.0 * cm, 12.2 * cm], font_size=8)]

    S += [paragraph("8. Verification", styles["H1x"])]
    verification = [
        ["Check", "Observed result"],
        ["Strict normal build", "GCC 13.3.0; all targets compile with warnings promoted to errors."],
        ["Unit suites", "Protocol, client, fault injection, Case 2 and enhanced range suites pass."],
        ["TCP end-to-end", "Part I, Case 1, Case 2 and Enhanced suites pass."],
        ["Fault behavior", "FIN and RST cuts, retries, resume, server restart, changed manifest and invalid topic covered."],
        ["Concurrency", "Six Case 1 clients, four Case 2 clients and multiple enhanced range workers verified."],
        ["Sanitizers", "AddressSanitizer and UndefinedBehaviorSanitizer pass for unit and all end-to-end suites."],
        ["Submission builds", "Part1 plus all three Part2 folders independently produce executable server and client."],
    ]
    S += [table(verification, [4.3 * cm, 11.9 * cm], font_size=8)]
    S += [paragraph("Representative commands", styles["H2x"])]
    S += [paragraph(
        "make clean &amp;&amp; make -j2 all<br/>"
        "make test &amp;&amp; make e2e &amp;&amp; make e2e_case1<br/>"
        "make e2e_case2 &amp;&amp; make e2e_enhanced<br/>"
        "make submission",
        styles["Codex"],
    )]
    S += [paragraph("The sanitizer configuration additionally used -fsanitize=address,undefined and -fno-omit-frame-pointer for every binary.", styles["Bodyx"])]

    S += [paragraph("9. Deviations, Limitations and Final Procedure", styles["H1x"])]
    deviations = [
        ["Item", "Final implementation / limitation"],
        ["Part I packaging", "Part1/ builds multi-threaded server by default; make single rebuilds the same required ./server name."],
        ["Server session persistence", "Session state is intentionally in memory. A server restart triggers safe client fallback to full GET."],
        ["Checkpoint cost", "Durable fsync-based checkpoints reduce re-sent data but are expensive on this validation host."],
        ["Enhanced persistence", "Range progress resumes per worker connection; final files publish only after complete SHA-256 verification."],
        ["Performance evidence", "Current results are local WSL2 validation, not the mandatory lab three-computer Animals-10 experiment."],
    ]
    S += [table(deviations, [4.1 * cm, 12.1 * cm], font_size=8)]
    S += [paragraph("Required final physical run", styles["H2x"])]
    S += [paragraph(
        "1. Copy the same commit to three Linux computers. 2. Put the Animals-10 raw-img root on the server computer. 3. Run the server on one machine and concurrent clients on the other two. 4. Repeat matched Part I and Part II sweeps, recording commit hash, host specifications, topic size, client count, p, N and seed. 5. Copy the raw CSVs into Results/, regenerate every graph, regenerate this PDF, then rerun make submission and inspect the extracted archive.",
        styles["Bodyx"],
    )]
    S += [paragraph(
        "Do not relabel the current loopback results as three-computer measurements. The code is submission-grade; the remaining evidence must come from the required physical setup.",
        styles["Callout"],
    )]
    S += [paragraph("10. Conclusion", styles["H1x"])]
    S += [paragraph(
        "The implementation satisfies the functional architecture of all four cases and has strong automated evidence for framing, integrity, safety, interruption handling, resumption and concurrency. Part I scales with concurrent clients on the validation host; Case 1 exposes restart waste; Case 2 bounds progress loss using validated checkpoints; and Enhanced Case 2 gains throughput by allowing independent ranges to proceed in parallel. Final submission should occur only after the team replaces or augments the local validation figures with the required three-computer Animals-10 experiment.",
        styles["Bodyx"],
    )]
    S += [paragraph("References", styles["H2x"])]
    S += [paragraph(
        "1. IT305 Lab: Socket Programming Project - Fault Tolerant Topic-based File Distribution Service.<br/>"
        "2. Design_Document_Group_064_091_.pdf.<br/>"
        "3. POSIX pthread and socket APIs; OpenSSL libcrypto SHA-256 implementation.",
        styles["Smallx"],
    )]

    doc.build(S)
    print(OUTPUT)


if __name__ == "__main__":
    build_report()
