import pandas as pd
import matplotlib.pyplot  as plt
import sys

'''
df_dram_30 = pd.read_csv("pr_web/autonuma_30/memory_trace_mapped_dram_pr_web.csv")
df_dram_50 = pd.read_csv("pr_web/autonuma_50/memory_trace_mapped_dram_pr_web.csv")
df_nvm_30 = pd.read_csv("pr_web/autonuma_30/memory_trace_mapped_nvm_pr_web.csv")
df_nvm_50 = pd.read_csv("pr_web/autonuma_50/memory_trace_mapped_nvm_pr_web.csv")

print("Pressure 30\n")
print(df_dram_30['call_stack_hash'].value_counts())
print(df_nvm_30['call_stack_hash'].value_counts())

print(df_dram_50['call_stack_hash'].value_counts())
print(df_nvm_50['call_stack_hash'].value_counts())

#print("\nTotal Samples(DRAM):", df_dram_30['call_stack_hash'].value_counts().sum(), " Cost(DRAM):", df_dram_30["access_weight"].sum())
#print("Total Samples(NVM):", df_nvm_30['call_stack_hash'].value_counts().sum(), " Cost total:", df_nvm_30["access_weight"].sum())
#press_30 = df_dram_30["access_weight"].sum() + df_nvm_30["access_weight"].sum()
#print("Total Cost(DRAM+NVM):", press_30)


print("\nPressure 50\n")
#print("\nTotal Samples:", df_dram_50['call_stack_hash'].value_counts().sum()," Cost total:", df_dram_50["access_weight"].sum())
#print("Total Samples(NVM):", df_nvm_50['call_stack_hash'].value_counts().sum(), " Cost total:", df_nvm_50["access_weight"].sum())
#press_50 = df_dram_50["access_weight"].sum() + df_nvm_50["access_weight"].sum()
#print("Total Cost(DRAM+NVM):", press_50)
#print("\n Press_30/Press_50 = ", round(press_30/press_50, 2))

'''


#sys.exit()
#call_stack_hash = 778186576
call_stack_hash = 796432943
#call_stack_hash = 1108841253
#call_stack_hash=1108841257

pressures = [30, 50]
for pressure in pressures:
    #df_track_info = pd.read_csv("pr_web/autonuma_30/track_info_pr_web.csv")
    file = "pr_web/autonuma_" + str(pressure) + "/track_info_pr_web.csv"
    df_track_info = pd.read_csv(file)

    #df_mmap = pd.read_csv("pr_web/autonuma_30/allocations_pr_web.csv", names=["ts_event","mmap","size","start_addr","call_stack_hash","call_stack"])
    file = "pr_web/autonuma_" + str(pressure) + "/allocations_pr_web.csv"
    df_mmap = pd.read_csv(file, names=["ts_event","mmap","size","start_addr","call_stack_hash","call_stack"])

    df_mmap = df_mmap.loc[df_mmap.call_stack_hash == call_stack_hash]

    fig, axes = plt.subplots(nrows=2, ncols=1, sharex=True, figsize=(4,2))

    if df_mmap.shape[0] > 1:
        start = df_mmap["ts_event"].iloc[0]
        #end = df_mmap["ts_event"].iloc[-1]

        mask = (df_track_info['timestamp'] >= start - 1) & (df_track_info['timestamp'] <= start + 1)
        df_track_info = df_track_info.loc[mask]

        df_track_info.set_index("timestamp", inplace=True)
        df_track_info[["dram_app"]].plot(kind="line", ax=axes[0])
        df_track_info[["pmem_app"]].plot(kind="line", ax=axes[1])
        axes[0].legend(['DRAM'], prop={'size': 8})
        axes[1].legend(['NVM'], prop={'size': 8})
        axes[0].set(xlabel=None)
        axes[1].set(xlabel=None)

        for index, row in df_mmap.iterrows():
            timestamp = row["ts_event"]
            axes[0].axvline(timestamp, color='red', linewidth = 0.75,linestyle ="--")
            axes[1].axvline(timestamp, color='red', linewidth = 0.75,linestyle ="--")
            break
    else:
        start = df_mmap["ts_event"].iloc[0]
        mask = (df_track_info['timestamp'] >= start - 5) & (df_track_info['timestamp'] <= start + 5)
        df_track_info = df_track_info.loc[mask]

        df_track_info.set_index("timestamp", inplace=True)
        df_track_info[["dram_app"]].plot(kind="line", ax=axes[0])
        df_track_info[["pmem_app"]].plot(kind="line", ax=axes[1])

        for index, row in df_mmap.iterrows():
            timestamp = row["ts_event"]
            axes[0].axvline(timestamp, color='red', linewidth = 0.75,linestyle ="--")
            axes[1].axvline(timestamp, color='red', linewidth = 0.75,linestyle ="--")

    plt.locator_params(axis='both', nbins=2)

    fig.text(-0.04, 0.5, "Memory Usage (MB)", ha='center', va='center', rotation='vertical')
    fig.text(0.5, -0.04, "Timestamp (ms)", ha='center', va='center')

    axes[1].ticklabel_format(useOffset=False, style='plain')
    filename = str(call_stack_hash) + "_pressure_" + str(pressure) + ".pdf"
    plt.savefig(filename, bbox_inches="tight")
