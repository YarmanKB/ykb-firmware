# Build

```bash
# Create NCS Workspace directory
mkdir -p ykb-firmware-ws

# Create python venv + source it
python3 -m venv ~/ykb-firmware-ws/venv
source ~/ykb-firmware-ws/venv/bin/activate

# Install west 
pip install -U pip wheel
pip install -U west

# Move to dir, really important
cd ykb-firmware-ws

# Initialize project, SDK and deps
west init -m https://github.com/YarmanKB/ykb-firmware --mr master
west update
west zephyr-export

# Install Python requirements for Zephyr scripts
pip install -r zephyr/scripts/requirements.txt

# Install toolchains
west sdk install

# Move to the project directory
cd ykb-firmware

# Build for the available board:
west ykb-build skadi 
```
