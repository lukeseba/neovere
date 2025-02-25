def read_font_from_qt_resource(resource_path):
    file = QFile(resource_path)
    if not file.open(QFile.ReadOnly):
        raise FileNotFoundError(f"Cannot open resource {resource_path}")

    # Write to a temporary file if needed
    temp_path = "/tmp/arial.ttf"  # Adjust for your OS
    with open(temp_path, "wb") as temp_file:
        temp_file.write(file.readAll())

    return temp_path

def generate_random_filename(length: int = 10, seed: int = None) -> str:
    """
    Generate a random string that can be safely used as a filename.
    :param length: Length of the random string (default is 10).
    :param seed: Seed value for reproducibility (default is None).
    :return: A randomly generated filename-safe string.
    """
    if seed is not None:
        random.seed(seed)

    characters = string.ascii_letters + string.digits
    return ''.join(random.choices(characters, k=length))

def set_openai_key(key: str):
    global api_key
    api_key = key
