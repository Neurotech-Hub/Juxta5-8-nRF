#!/usr/bin/env python3

import sys
import os
import argparse

def main():
    parser = argparse.ArgumentParser(description='List subdirectories of a given directory')
    parser.add_argument('--directory', required=True, help='Directory to scan for subdirectories')
    parser.add_argument('--out-file', required=True, help='Output file to write subdirectories')
    parser.add_argument('--trigger-file', required=True, help='Trigger file to create after processing')
    
    args, remaining_args = parser.parse_known_args()
    
    base_dir = args.directory
    out_file = args.out_file
    trigger_file = args.trigger_file
    
    # Create output directory if it doesn't exist
    os.makedirs(os.path.dirname(out_file), exist_ok=True)
    if trigger_file:
        os.makedirs(os.path.dirname(trigger_file), exist_ok=True)
    
    subdirs = []
    if os.path.isdir(base_dir):
        for item in os.listdir(base_dir):
            item_path = os.path.join(base_dir, item)
            if os.path.isdir(item_path):
                subdirs.append(os.path.join(base_dir, item))
    
    # Write subdirectories to output file
    with open(out_file, 'w') as f:
        f.write('\n'.join(sorted(subdirs)))
    
    # Create trigger file
    if trigger_file:
        with open(trigger_file, 'w') as f:
            f.write("")

if __name__ == "__main__":
    main() 