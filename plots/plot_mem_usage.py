import pandas as pd
import matplotlib.pyplot as plt
import glob
import sys
import os

schedule_type = str(sys.argv[1])

def plot_memory_usage():
    files = glob.glob('track_info_*.csv')
    name = files[0].split('.')[0]
    app_dataset = name.split('_')[-2] + "_" + name.split('_')[-1]

    #dataframe of memory consumption in DRAM and NVM
    df = pd.read_csv(files[0])
    exec_time = round(df['timestamp'].iloc[-1] - df['timestamp'].iloc[0],1)
    os.system("echo %s > exec_time.txt"%exec_time)
    start_point = df['timestamp'].iloc[0]

    df["timestamp_v2"] = df['timestamp'] - start_point
    df.drop(["timestamp"], axis=1, inplace=True)
    df.set_index('timestamp_v2', inplace=True)

    df['dram_app'] = df['dram_app']/1000
    df['nvm_app'] = df['nvm_app']/1000

    #dataframe of samples
    df_external_access = pd.read_csv("loads.txt", names=["ts_event", "virt_addr", "mem_access"])
    df_external_access["timestamp_v2"] = df_external_access['ts_event'] - start_point
    df_external_access['timestamp_v2'] = df_external_access['timestamp_v2'].astype(int)
    df_external_access.drop(["ts_event"], axis=1, inplace=True)

    df_dram = df_external_access.loc[df_external_access.mem_access == "DRAM_hit"]    
    df_nvm = df_external_access.loc[df_external_access.mem_access == "NVM_hit"]

    df_dram = df_dram.groupby(['timestamp_v2']).size().reset_index(name='DRAM')
    df_dram.set_index("timestamp_v2", inplace=True)
    df_nvm = df_nvm.groupby(['timestamp_v2']).size().reset_index(name='NVM')
    df_nvm.set_index("timestamp_v2", inplace=True)
    
    #Plot
    fig, axes = plt.subplots(figsize=(5,4),nrows=2,sharex=True)
    
    #First subplot
    df[["dram_app"]].plot(linewidth=0.75, ax=axes[0])
    df[["nvm_app"]].plot(style="-.",linewidth=0.75, ax=axes[0])
    axes[0].legend(['DRAM','NVM'], prop={'size': 10}, ncol=2, fancybox=True, framealpha=0.5, bbox_to_anchor=(0.68, 1.61))
    axes[0].tick_params(axis='x', rotation=45)
    #ax.set_xlabel('Timestamp(seconds)')
    axes[0].set_ylabel('Memory \n Consumption (GB)')
    
    #Second subplot
    df_dram["DRAM"].plot(ax=axes[1], linewidth=0.75, color="tab:blue")
    df_nvm["NVM"].plot(ax=axes[1], style="-.",linewidth=0.75, color="tab:orange")
    axes[1].set_ylabel('Number of Load \n Access')
    axes[1].set_xlabel("Timestamp")

    factor = str(round(df_nvm.NVM.sum()/df_dram.DRAM.sum(),1))
    label = "Exec.Time:" + str(exec_time) + "\n" +  "DRAM samples:" + str(df_dram.DRAM.sum()) + "\n" + "NVM   samples:" + str(df_nvm.NVM.sum()) + " (" + factor + "x)"
    axes[0].annotate(label, xy=(0.20, 1.05), xycoords='axes fraction')

    axes[1].ticklabel_format(style='sci',scilimits=(0,0),axis='y')

    filename = "mem_usage_" + app_dataset + "_" + schedule_type + ".pdf"
    plt.savefig(filename, bbox_inches="tight")
    plt.clf()

plot_memory_usage()
