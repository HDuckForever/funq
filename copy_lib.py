import shutil
import glob

build_dir =  "./build/libFunq"
run_script_dir = "./server/funq_server"

lib_lst = [ "libFunqStatic.a", "libFunq.so" ]


def copy_file(source_path, destination_path):
    file_lst = glob.glob(source_path)
    if len(file_lst) == 0:
        print("warning: file(s) '{0}' not found".format(source_path))
        return False

    for file in file_lst:
        print("Copy {0} -> {1}".format(file, destination_path))
    try:
        shutil.copy(file, destination_path)
    except Exception as error:
        print(error)
        return False

    return True

for lib in lib_lst:
    copy_file(f"{build_dir}/{lib}", f"{run_script_dir}/{lib}")