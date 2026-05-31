#!/usr/bin/python3
import json
import sys
import argparse

def parse_args():
    parser = argparse.ArgumentParser(description='Compare two Csilk benchmark results.')
    parser.add_argument('baseline', help='Path to baseline JSON file')
    parser.add_argument('current', help='Path to current JSON file')
    parser.add_argument('--threshold', type=float, default=10.0, help='Regression threshold percentage')
    parser.add_argument('--markdown', help='Path to output markdown report')
    return parser.parse_args()

def load_json(path):
    try:
        with open(path, 'r') as f:
            return json.load(f)
    except Exception as e:
        print(f"Error loading {path}: {e}")
        sys.exit(1)

def main():
    args = parse_args()
    baseline = load_json(args.baseline)
    current = load_json(args.current)

    b_results = {r['name']: r for r in baseline['results']}
    c_results = {r['name']: r for r in current['results']}

    regression_detected = False
    report = []
    report.append("# Benchmark Comparison Report")
    report.append("")
    report.append("| Test | Baseline (RPS) | Current (RPS) | Change (%) | Status |")
    report.append("| :--- | :---: | :---: | :---: | :---: |")

    print(f"{'Test':<20} {'Baseline':>12} {'Current':>12} {'Change':>10}")
    print("-" * 60)

    for name in c_results:
        c_rps = float(c_results[name]['rps'])
        b_rps = float(b_results[name]['rps']) if name in b_results else 0
        
        change = 0
        if b_rps > 0:
            change = ((c_rps - b_rps) / b_rps) * 100
        
        status = "✅"
        if change < -args.threshold:
            status = "❌"
            regression_detected = True
        elif change > args.threshold:
            status = "🚀"

        print(f"{name:<20} {b_rps:>12.0f} {c_rps:>12.0f} {change:>9.2f}% {status}")
        report.append(f"| {name} | {b_rps:,.0f} | {c_rps:,.0f} | {change:+.2f}% | {status} |")

    if args.markdown:
        with open(args.markdown, 'w') as f:
            f.write("\n".join(report))
        print(f"\nMarkdown report saved to: {args.markdown}")

    if regression_detected:
        print("\nFAIL: Significant performance regression detected!")
        sys.exit(1)
    else:
        print("\nPASS: Performance is within acceptable limits.")
        sys.exit(0)

if __name__ == "__main__":
    main()
