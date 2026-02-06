# Installing Ghidra on macOS

## Step 1: Install Java (Required)

Ghidra requires Java 17 or later. Install via Homebrew:

```bash
brew install openjdk@17
```

Then link it so your system can find it:

```bash
sudo ln -sfn $(brew --prefix)/opt/openjdk@17/libexec/openjdk.jdk /Library/Java/JavaVirtualMachines/openjdk-17.jdk
```

Verify installation:

```bash
java -version
```

You should see something like:
```
openjdk version "17.0.x" ...
```

## Step 2: Download Ghidra

Latest version (12.0.1):

```bash
cd ~/Downloads
curl -L -O https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.0.1_build/ghidra_12.0.1_PUBLIC_20260114.zip
```

## Step 3: Extract Ghidra

```bash
unzip ghidra_12.0.1_PUBLIC_20260114.zip
mv ghidra_12.0.1_PUBLIC /Applications/
```

## Step 4: Launch Ghidra

```bash
/Applications/ghidra_12.0.1_PUBLIC/ghidraRun
```

Or create an alias in your `~/.zshrc` or `~/.bashrc`:

```bash
echo 'alias ghidra="/Applications/ghidra_12.0.1_PUBLIC/ghidraRun"' >> ~/.zshrc
source ~/.zshrc
```

Then you can just run:
```bash
ghidra
```

## Quick Setup Script

Here's a one-liner to do it all (after installing Java):

```bash
cd ~/Downloads && \
curl -L -O https://github.com/NationalSecurityAgency/ghidra/releases/download/Ghidra_12.0.1_build/ghidra_12.0.1_PUBLIC_20260114.zip && \
unzip ghidra_12.0.1_PUBLIC_20260114.zip && \
mv ghidra_12.0.1_PUBLIC /Applications/ && \
echo 'alias ghidra="/Applications/ghidra_12.0.1_PUBLIC/ghidraRun"' >> ~/.zshrc && \
source ~/.zshrc && \
echo "✅ Ghidra installed! Run 'ghidra' to launch."
```

## Troubleshooting

### "Java Runtime not found"
Make sure you ran the `sudo ln -sfn` command above.

### "Permission denied" on ghidraRun
```bash
chmod +x /Applications/ghidra_12.0.1_PUBLIC/ghidraRun
```

### macOS "unidentified developer" warning
Right-click on ghidraRun and select "Open" the first time, then click "Open" in the security dialog.

## Next Steps

Once Ghidra is installed, follow the guide in `ghidra_quickstart.md` to:
1. Create a new project
2. Import your firmware binaries
3. Start analyzing!
