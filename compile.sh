PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
cmake -S "$PROJECT_DIR" -B "$PROJECT_DIR/Build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=1
cd "$PROJECT_DIR/Build"
make -j8