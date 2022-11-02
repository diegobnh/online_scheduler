import pandas as pd
import matplotlib.pyplot as plt
import glob

def plot_memory_usage():
    files = glob.glob('track_info_*.csv')
    name = files[0].split('.')[0]
    app_dataset = name.split('_')[-2] + "_" + name.split('_')[-1]

    df = pd.read_csv(files[0])
    exec_time = round(df['timestamp'].iloc[-1] - df['timestamp'].iloc[0],1)
    print(exec_time)
    df.set_index('timestamp', inplace=True)

    df['dram_page_cache'] = df['dram_page_cache_active'] + df['dram_page_cache_inactive']
    #df['dram_page_cache']  = df['dram_page_cache']/1000000
    #df['dram_page_cache'] = df['dram_page_cache'].round(2)

    df['dram_app'] = df['dram_app']/1000
    df['pmem_app'] = df['pmem_app']/1000

    fig = plt.figure()

    ax = df[["dram_app"]].plot(linewidth=0.75)
    df[["pmem_app"]].plot(ax=ax, style="-.",linewidth=0.75)
    ax.legend(['DRAM (App)','NVM (App)'], prop={'size': 10}, ncol=2, fancybox=True, framealpha=0.5, bbox_to_anchor=(0.75, 1.21))
    ax.tick_params(axis='x', rotation=45)
    ax.set_xlabel('Timestamp(seconds)')
    ax.set_ylabel('Memory Consumption (GB)')

    label = "Exec.Time:" + str(exec_time)
    ax.annotate(label, xy=(0.35, 1.05), xycoords='axes fraction')

    filename = "mem_usage" + app_dataset + ".pdf"
    plt.savefig(filename, bbox_inches="tight")
    plt.clf()

plot_memory_usage()
